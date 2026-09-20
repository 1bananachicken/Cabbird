#include "cabbird/memory.hpp"

#include "cabbird/thread_local_value.hpp"

#include <Psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

namespace cabbird::mem {
namespace {

// ============================================================================================
// THE PROVEN-RANGE CACHE, and why it is the single largest cost in the ESP.
// ============================================================================================
//
// Every read the entity walk made went through TWO kernel transitions: one `VirtualQuery` in
// `HasRange` to prove the target was committed and readable, and one `ReadProcessMemory` to
// actually move the bytes.  The walk performs a dozen or more reads PER ENTITY, on the game
// thread, once per refresh.  That is where the measured 71.6 ms of a single game tick went --
// and it is why four rounds of optimising the position CALL changed almost nothing.
//
// When the target is our own address space -- which it always is here, because this is an
// injected image reading its host -- a validated read needs no transition at all.  A plain
// `memcpy` from a page that `HasRange` has already shown to be committed and readable returns
// exactly the bytes `ReadProcessMemory` would have returned, so the transition is pure overhead.
//
// A cache collapses almost all of it: IL2CPP objects and the runtime's metadata tables live in a
// handful of large, long-lived regions, so consecutive reads nearly always land in the same one.
// Entries are only ever created from a SUCCESSFUL `VirtualQuery`, and an address outside every
// entry falls back to the fully checked path.
//
// FOUR THINGS THE FIRST VERSION OF THIS CACHE GOT WRONG.  All four come from the sibling
// project's two follow-up commits to the same cache (Anomaly 6975770, 393e4ca), and each one is
// a property of the code, not a preference:
//
//   * A ONE-SECOND TTL.  The first version had none, so an entry lived until the 8 slots rotated
//     it out -- and the comment claimed "a region that is unmapped simply stops being served from
//     cache rather than being trusted", which was FALSE: nothing invalidated anything, so a
//     region freed after it was validated kept being trusted for as long as the process ran.
//     Entries now expire together, once a second, and the worst case is a re-validation.
//
//   * PER THREAD, VIA FLS.  The first version was a namespace-scope array with a rotating index:
//     shared mutable state written by the game thread's entity walk while the render thread and
//     worker threads read it, with no synchronisation at all.  Two 8-byte stores landing in a
//     slot in the other order (new `base`, stale `end`) produce a range that was NEVER validated,
//     and a read inside it is not a failed read -- it is `memcpy` from unmapped memory, i.e. an
//     access violation in the game.  Per-thread state removes the race rather than narrowing it.
//     It is `ThreadLocalObject` (FLS) and not `thread_local`: this image is manually mapped and
//     must carry no TLS directory -- see include/cabbird/thread_local_value.hpp.
//
//   * REMEMBERED FROM THE QUERY THE LOOP ALREADY RAN.  The first version called `VirtualQuery` a
//     second time for a region it had just queried, i.e. one wasted kernel transition per
//     newly-seen region.
//
//   * WRITABILITY IS RECORDED.  The first version cached readable regions only, so every write
//     paid the full walk; the `writable` flag comes free with the query.
//
// WHAT MAKES A WRONG "READABLE" VERDICT SAFE HERE, stated for THIS design rather than Anomaly's:
// Anomaly's `ReadMemoryInto` calls `ReadProcessMemory`, which fails on a bad address, so its TTL
// only has to bound staleness.  This one calls `memcpy`, which FAULTS.  The TTL bounds the
// staleness, single-region containment keeps the middle of a range from going unverified, and the
// copy itself is SEH-guarded (`CopyGuarded` below) so that the residual TOCTOU window -- a region
// unmapped between the query and the copy -- yields "unreadable" instead of taking the game down.
// That is the property src/game/unity/unity_adapter.cpp's read helper documents.
struct ProvenRange {
    std::uintptr_t base{};
    std::uintptr_t end{};
    bool writable{};
};

struct RangeCacheState {
    // Sorted by `base`; a binary search replaces the first version's linear scan of 8 slots.
    std::vector<ProvenRange> ranges;
    std::chrono::steady_clock::time_point stamp{};
};

constexpr auto kRangeCacheTtl = std::chrono::seconds(1);
// A bound on growth, so a full-region scan (the dumper walks every region of the process) cannot
// turn this into an unbounded vector with an O(n) insert per region. Past the cap, regions are
// simply not remembered -- reads still take the checked path, which is what they did before.
constexpr std::size_t kRangeCacheCapacity = 1024;

// The core is manually mapped and cannot carry loader-managed static TLS, so per-thread state
// goes through FLS.  Declared at namespace scope: the mapper runs the PE entry point, so this
// constructor does run.
ThreadLocalObject<RangeCacheState> g_proven_ranges;

// True when the WHOLE range `[address, address + size)` lies inside ONE proven region.  Requiring
// a single region rather than merely "both ends are cached" matters: two ends in two different
// entries would leave the middle unverified, which is precisely the kind of gap this cache exists
// to not introduce.
bool InProvenRange(
    const RangeCacheState& state, std::uintptr_t address, std::size_t size, bool write) noexcept {
    const auto position = std::upper_bound(
        state.ranges.begin(), state.ranges.end(), address,
        [](std::uintptr_t value, const ProvenRange& range) { return value < range.base; });
    if (position == state.ranges.begin()) return false;
    const ProvenRange& range = *(position - 1);
    return address >= range.base && address + size <= range.end && (!write || range.writable);
}

// Records a region the caller has just proven readable (and, when `writable`, writable).
void RememberProvenRange(
    RangeCacheState& state, std::uintptr_t base, std::uintptr_t end, bool writable) {
    if (base >= end) return;
    const auto position = std::lower_bound(
        state.ranges.begin(), state.ranges.end(), base,
        [](const ProvenRange& range, std::uintptr_t value) { return range.base < value; });
    if (position != state.ranges.end() && position->base == base) {
        position->end = (std::max)(position->end, end);
        // Only every observation agreeing keeps a region writable: a later readable-only sighting
        // must not leave an earlier write permission standing.
        position->writable = position->writable && writable;
        return;
    }
    if (state.ranges.size() >= kRangeCacheCapacity) return;
    state.ranges.insert(position, ProvenRange{base, end, writable});
}

// MSVC rejects `__try` in a function that needs C++ object unwinding (C2712), so the guard is its
// own function whose only locals are PODs -- the same split src/mem/il2cpp_dump.cpp uses.  Only an
// access violation is swallowed; anything else keeps propagating, because this layer has no
// business hiding a stack overflow or an illegal instruction.
bool CopyGuarded(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (::GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool HasRange(std::uintptr_t address, std::size_t size, bool write) {
    if (address == 0 || size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }
    const auto end = address + size;
    auto& state = g_proven_ranges.Get();
    const auto now = std::chrono::steady_clock::now();
    if (state.stamp.time_since_epoch().count() == 0 || now - state.stamp > kRangeCacheTtl) {
        state.ranges.clear();
        state.stamp = now;
    }
    // THE FAST PATH, for reads and for writes that were proven writable.  A hit pays no kernel
    // transition at all, which is the entire point of the cache.
    if (InProvenRange(state, address, size, write)) {
        return true;
    }
    auto cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        const DWORD base = info.Protect & 0xff;
        const bool readable = base == PAGE_READONLY || base == PAGE_READWRITE ||
                              base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
                              base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
        const bool writable = base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
                              base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (write && !writable)) return false;
        const auto region_base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto region_end = region_base + info.RegionSize;
        if (region_end <= cursor) return false;
        // Remembered here, at the one place a region is actually proven readable, and from THIS
        // query: the whole region shares one protection, so proving the part the cursor is in
        // proves all of it.
        RememberProvenRange(state, region_base, region_end, writable);
        cursor = std::min(region_end, end);
    }
    return true;
}

template <typename T>
bool ReadLocal(std::uintptr_t address, T& output) {
    SIZE_T read{};
    return ReadProcessMemory(
               GetCurrentProcess(), reinterpret_cast<const void*>(address),
               &output, sizeof(output), &read) != FALSE &&
           read == sizeof(output);
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool EqualInsensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
           _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

struct ImageLayout final {
    std::size_t image_size{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

std::optional<ImageLayout> ReadMemoryImageLayout(const ModuleInfo& module) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadLocal(module.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 ||
        module.base > std::numeric_limits<std::uintptr_t>::max() -
            static_cast<std::uintptr_t>(dos.e_lfanew)) {
        return std::nullopt;
    }
    const auto nt_address = module.base + static_cast<std::uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadLocal(nt_address, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > 96) {
        return std::nullopt;
    }

    const auto section_address = nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    ImageLayout layout;
    layout.image_size = nt.OptionalHeader.SizeOfImage;
    layout.sections.reserve(nt.FileHeader.NumberOfSections);
    for (unsigned index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER header{};
        if (!ReadLocal(section_address + index * sizeof(header), header)) {
            return std::nullopt;
        }
        layout.sections.push_back(header);
    }
    return layout;
}

std::optional<ImageLayout> ReadFileImageLayout(const std::wstring& path) {
    if (path.empty()) return std::nullopt;
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) return std::nullopt;

    IMAGE_DOS_HEADER dos{};
    stream.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return std::nullopt;
    }
    stream.seekg(dos.e_lfanew, std::ios::beg);
    IMAGE_NT_HEADERS64 nt{};
    stream.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!stream || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > 96) {
        return std::nullopt;
    }

    const std::streamoff section_offset = static_cast<std::streamoff>(dos.e_lfanew) +
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    stream.seekg(section_offset, std::ios::beg);
    if (!stream) return std::nullopt;
    ImageLayout layout;
    layout.image_size = nt.OptionalHeader.SizeOfImage;
    layout.sections.resize(nt.FileHeader.NumberOfSections);
    stream.read(
        reinterpret_cast<char*>(layout.sections.data()),
        static_cast<std::streamsize>(layout.sections.size() * sizeof(IMAGE_SECTION_HEADER)));
    return stream ? std::optional<ImageLayout>(std::move(layout)) : std::nullopt;
}

}  // namespace

std::vector<ModuleInfo> EnumerateModules() {
    std::vector<HMODULE> handles(256);
    DWORD needed{};
    while (true) {
        if (!K32EnumProcessModules(
                GetCurrentProcess(), handles.data(),
                static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) {
            return {};
        }
        if (needed <= handles.size() * sizeof(HMODULE)) {
            handles.resize(needed / sizeof(HMODULE));
            break;
        }
        handles.resize(needed / sizeof(HMODULE) + 16);
    }

    std::vector<ModuleInfo> modules;
    modules.reserve(handles.size());
    for (const auto handle : handles) {
        MODULEINFO native{};
        std::array<wchar_t, 32768> path{};
        if (!K32GetModuleInformation(GetCurrentProcess(), handle, &native, sizeof(native))) {
            continue;
        }
        const DWORD length = K32GetModuleFileNameExW(
            GetCurrentProcess(), handle, path.data(), static_cast<DWORD>(path.size()));
        ModuleInfo module;
        module.path.assign(path.data(), length);
        module.name = BaseName(module.path);
        module.base = reinterpret_cast<std::uintptr_t>(native.lpBaseOfDll);
        module.size = native.SizeOfImage;
        modules.push_back(std::move(module));
    }
    std::sort(modules.begin(), modules.end(), [](const auto& left, const auto& right) {
        return left.base < right.base;
    });
    return modules;
}

std::optional<ModuleInfo> FindModule(std::wstring_view name) {
    const auto modules = EnumerateModules();
    if (name.empty()) {
        const auto main_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto main_module =
            std::find_if(modules.begin(), modules.end(), [&](const auto& module) {
                return module.base == main_base;
            });
        return main_module == modules.end() ? std::nullopt : std::optional<ModuleInfo>(*main_module);
    }
    for (const auto& module : modules) {
        if (EqualInsensitive(module.name, name) || EqualInsensitive(module.path, name)) {
            return module;
        }
    }
    return std::nullopt;
}

std::vector<SectionInfo> EnumerateSections(const ModuleInfo& module) {
    auto layout = ReadMemoryImageLayout(module);
    if (!layout) layout = ReadFileImageLayout(module.path);
    if (!layout) return {};
    const std::size_t image_size = module.size != 0 ? module.size : layout->image_size;
    std::vector<SectionInfo> sections;
    sections.reserve(layout->sections.size());
    for (const IMAGE_SECTION_HEADER& header : layout->sections) {
        std::array<char, IMAGE_SIZEOF_SHORT_NAME + 1> name{};
        std::memcpy(name.data(), header.Name, IMAGE_SIZEOF_SHORT_NAME);
        const auto offset = static_cast<std::size_t>(header.VirtualAddress);
        const auto size = static_cast<std::size_t>(header.Misc.VirtualSize);
        if (offset >= image_size) continue;
        sections.push_back({
            name.data(), module.base + offset, std::min(size, image_size - offset),
            header.Characteristics});
    }
    return sections;
}

std::vector<RegionInfo> EnumerateRegions(const ModuleInfo& module) {
    std::vector<RegionInfo> regions;
    const auto end = module.base + module.size;
    auto cursor = module.base;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 ||
            info.RegionSize == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto region_end = base + info.RegionSize;
        const auto clipped_end = std::min(region_end, end);
        regions.push_back({base, clipped_end - base, info.State, info.Protect, info.Type});
        if (region_end <= cursor) {
            break;
        }
        cursor = region_end;
    }
    return regions;
}

std::vector<RegionInfo> EnumerateProcessRegions() {
    std::vector<RegionInfo> regions;
    auto cursor = std::uintptr_t{0};
    // VirtualQuery walks the address space one region at a time, and returns 0
    // once we run past the highest reservation -- the natural loop terminator.
    // Advancing by RegionSize means the call count is the number of distinct
    // regions (a few thousand for a Unity game), not the size of the address
    // space, so this is cheap even though it looks like it scans 128 TB.
    while (true) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 ||
            info.RegionSize == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.State == MEM_COMMIT) {
            regions.push_back({base, info.RegionSize, info.State, info.Protect, info.Type});
        }
        const auto next = base + info.RegionSize;
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }
    return regions;
}

std::vector<std::uintptr_t> ScanSection(
    const ModuleInfo& module,
    std::string_view section_name,
    const Pattern& pattern,
    std::size_t limit) {
    const auto sections = EnumerateSections(module);
    const auto found = std::find_if(sections.begin(), sections.end(), [&](const auto& section) {
        return section.name == section_name;
    });
    if (found == sections.end() || found->virtual_size == 0) {
        return {};
    }
    if (!HasRange(found->base, found->virtual_size, false)) return {};
    const auto* const mapped = reinterpret_cast<const std::uint8_t*>(found->base);
    const auto offsets = pattern.FindAll(
        std::span<const std::uint8_t>(mapped, found->virtual_size), limit);
    std::vector<std::uintptr_t> addresses;
    addresses.reserve(offsets.size());
    for (const auto offset : offsets) {
        addresses.push_back(found->base + offset);
    }
    return addresses;
}

std::vector<std::uintptr_t> ScanExecutableSections(
    const ModuleInfo& module,
    const Pattern& pattern,
    std::size_t limit) {
    std::vector<std::uintptr_t> addresses;
    for (const auto& section : EnumerateSections(module)) {
        if (addresses.size() >= limit) {
            break;
        }
        if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.virtual_size == 0) {
            continue;
        }
        if (!HasRange(section.base, section.virtual_size, false)) {
            continue;
        }
        const auto* const mapped = reinterpret_cast<const std::uint8_t*>(section.base);
        const auto offsets = pattern.FindAll(
            std::span<const std::uint8_t>(mapped, section.virtual_size), limit - addresses.size());
        for (const auto offset : offsets) {
            addresses.push_back(section.base + offset);
        }
    }
    return addresses;
}

std::optional<std::uintptr_t> ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend) {
    std::int32_t displacement{};
    if (!ReadLocal(instruction + displacement_offset, displacement)) {
        return std::nullopt;
    }
    return instruction + instruction_size + displacement + addend;
}

std::optional<std::vector<std::uint8_t>> ReadMemory(std::uintptr_t address, std::size_t size) {
    if (size == 0 || size > 4096) return std::nullopt;
    std::vector<std::uint8_t> bytes(size);
    if (!ReadMemoryInto(address, bytes.data(), size)) return std::nullopt;
    return bytes;
}

bool ReadMemoryInto(std::uintptr_t address, void* destination, std::size_t size) {
    if (destination == nullptr || !HasRange(address, size, false)) return false;
    // A read of our OWN address space, from a range `HasRange` has just proven committed and
    // readable, needs no kernel transition: `ReadProcessMemory(GetCurrentProcess(), ...)` would
    // copy exactly these bytes, because every page in the range is present and readable, so it
    // cannot partially fail.  This single change is what removes two transitions per read from
    // the entity walk -- the measured cost that made one game tick 71.6 ms.
    //
    // The copy is SEH-guarded because `memcpy` faults where `ReadProcessMemory` would have
    // returned FALSE: the range cache's one-second TTL bounds how stale a "readable" verdict can
    // be, and this contains the window that remains (a region unmapped between the query and the
    // copy).  Without it, one stale entry is an access violation in the game rather than a read
    // that reports "unreadable".
    return CopyGuarded(destination, reinterpret_cast<const void*>(address), size);
}

bool WriteMemory(std::uintptr_t address, const void* source, std::size_t size) {
    if (source == nullptr || !HasRange(address, size, true)) return false;
    SIZE_T written{};
    const bool result = WriteProcessMemory(
                            GetCurrentProcess(), reinterpret_cast<void*>(address), source, size,
                            &written) != FALSE &&
                        written == size;
    if (result) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return result;
}

bool PatchMemory(std::uintptr_t address, const void* source, std::size_t size) {
    if (source == nullptr || !HasRange(address, size, false)) return false;
    struct ProtectedSpan {
        void* address{};
        std::size_t size{};
        DWORD previous{};
    };
    std::vector<ProtectedSpan> spans;
    const auto end = address + size;
    auto cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) break;
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        ProtectedSpan span{
            reinterpret_cast<void*>(cursor), std::min(region_end, end) - cursor, 0};
        if (!VirtualProtect(span.address, span.size, PAGE_EXECUTE_READWRITE, &span.previous)) break;
        spans.push_back(span);
        cursor += span.size;
    }
    if (cursor != end) {
        for (auto iterator = spans.rbegin(); iterator != spans.rend(); ++iterator) {
            DWORD ignored{};
            VirtualProtect(iterator->address, iterator->size, iterator->previous, &ignored);
        }
        return false;
    }
    SIZE_T written{};
    const bool wrote = WriteProcessMemory(
                           GetCurrentProcess(), reinterpret_cast<void*>(address), source, size,
                           &written) != FALSE &&
                       written == size;
    if (wrote) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    bool restored = true;
    for (auto iterator = spans.rbegin(); iterator != spans.rend(); ++iterator) {
        DWORD ignored{};
        restored =
            VirtualProtect(iterator->address, iterator->size, iterator->previous, &ignored) != FALSE &&
            restored;
    }
    return wrote && restored;
}

bool ProtectMemory(std::uintptr_t address, std::size_t size, DWORD protection, DWORD& previous) {
    if (address == 0 || size == 0) return false;
    return VirtualProtect(reinterpret_cast<void*>(address), size, protection, &previous) != FALSE;
}

void* AllocateMemory(std::size_t size, DWORD protection) {
    if (size == 0) return nullptr;
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protection);
}

bool FreeMemory(void* address) {
    return address != nullptr && VirtualFree(address, 0, MEM_RELEASE) != FALSE;
}

std::optional<std::uintptr_t> ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) {
    if (base == 0 || (count != 0 && offsets == nullptr) || count > 64) return std::nullopt;
    auto current = base;
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t next{};
        if (!ReadMemoryInto(current, &next, sizeof(next)) || next == 0) return std::nullopt;
        const auto offset = offsets[index];
        if (offset >= 0) {
            const auto magnitude = static_cast<std::uintptr_t>(offset);
            if (next > std::numeric_limits<std::uintptr_t>::max() - magnitude) return std::nullopt;
            current = next + magnitude;
        } else {
            const auto magnitude = std::uintptr_t{0} - static_cast<std::uintptr_t>(offset);
            if (next < magnitude) return std::nullopt;
            current = next - magnitude;
        }
    }
    return current;
}

std::string ProtectionName(DWORD protection) {
    if (protection == 0) return "none";
    const DWORD base = protection & 0xff;
    std::string result;
    switch (base) {
    case PAGE_NOACCESS: result = "noaccess"; break;
    case PAGE_READONLY: result = "r"; break;
    case PAGE_READWRITE: result = "rw"; break;
    case PAGE_WRITECOPY: result = "wc"; break;
    case PAGE_EXECUTE: result = "x"; break;
    case PAGE_EXECUTE_READ: result = "rx"; break;
    case PAGE_EXECUTE_READWRITE: result = "rwx"; break;
    case PAGE_EXECUTE_WRITECOPY: result = "xwc"; break;
    default: result = "unknown"; break;
    }
    if ((protection & PAGE_GUARD) != 0) result += "|guard";
    if ((protection & PAGE_NOCACHE) != 0) result += "|nocache";
    if ((protection & PAGE_WRITECOMBINE) != 0) result += "|writecombine";
    return result;
}

std::string StateName(DWORD state) {
    switch (state) {
    case MEM_COMMIT: return "commit";
    case MEM_RESERVE: return "reserve";
    case MEM_FREE: return "free";
    default: return "unknown";
    }
}

std::string TypeName(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return "image";
    case MEM_MAPPED: return "mapped";
    case MEM_PRIVATE: return "private";
    default: return "unknown";
    }
}

}  // namespace cabbird::mem
