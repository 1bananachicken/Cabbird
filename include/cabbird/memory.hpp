// Cabbird's process-memory layer: module/section/region enumeration, pattern scanning,
// read/write/patch/protect, allocation, and pointer-chain resolution.
//
// DERIVED FROM Anomaly's include/memory.hpp, but this file is a RECONSTRUCTION and must be
// labelled as one.  The original Cabbird version of this header was destroyed during this
// migration session by a namespace-rewrite script that replaced the file's tail instead of
// inserting into it; the file is not under git, so it could not be restored from history.
//
// It was rebuilt from the two sources that are authoritative and still present:
//   * `src/mem/memory.cpp` -- every signature below is copied from the definition, not from
//     memory, and the namespace is `cabbird::mem` because that is what the implementation
//     opens.
//   * `include/cabbird/pattern.hpp` -- `Pattern` / `PatternByte`.
// Anything not found in either is not invented here; if a caller needs a symbol that is
// missing, the linker will say so rather than this file guessing.
#pragma once

#include "cabbird/pattern.hpp"

// `unitymem`, Anomaly's name for this layer, is aliased in cabbird/unitymem_compat.hpp.
// That header is the single place the alias is declared, so the name can never resolve to
// two different targets.  See its comment for the full rationale.

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird::mem {

struct ModuleInfo {
    std::wstring name;
    std::wstring path;
    std::uintptr_t base{};
    std::size_t size{};
};

struct SectionInfo {
    std::string name;
    std::uintptr_t base{};
    std::size_t virtual_size{};
    DWORD characteristics{};
};

struct RegionInfo {
    std::uintptr_t base{};
    std::size_t size{};
    DWORD state{};
    DWORD protection{};
    DWORD type{};
};

std::vector<ModuleInfo> EnumerateModules();
std::optional<ModuleInfo> FindModule(std::wstring_view name);
std::vector<SectionInfo> EnumerateSections(const ModuleInfo& module);
std::vector<RegionInfo> EnumerateRegions(const ModuleInfo& module);
std::vector<RegionInfo> EnumerateProcessRegions();

std::vector<std::uintptr_t> ScanSection(
    const ModuleInfo& module,
    std::string_view section_name,
    const Pattern& pattern,
    std::size_t limit);
std::vector<std::uintptr_t> ScanExecutableSections(
    const ModuleInfo& module,
    const Pattern& pattern,
    std::size_t limit);

std::optional<std::uintptr_t> ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend);

std::optional<std::vector<std::uint8_t>> ReadMemory(std::uintptr_t address, std::size_t size);
/* Copies `size` bytes out of this process, or reports false.
 *
 * CONTRACT, because the two halves are a pair: the range check is a per-thread cache of regions
 * already proven readable (one-second TTL, FLS-backed -- see src/mem/memory.cpp), and the copy
 * itself is SEH-guarded, so a pointer that goes stale between the check and the copy yields false
 * instead of faulting.  Callers that follow pointers out of managed memory rely on exactly that:
 * an IL2CPP walk takes pointers that another thread may have freed a moment ago. */
bool ReadMemoryInto(std::uintptr_t address, void* destination, std::size_t size);
bool WriteMemory(std::uintptr_t address, const void* source, std::size_t size);
bool PatchMemory(std::uintptr_t address, const void* source, std::size_t size);
bool ProtectMemory(std::uintptr_t address, std::size_t size, DWORD protection, DWORD& previous);
void* AllocateMemory(std::size_t size, DWORD protection = PAGE_READWRITE);
bool FreeMemory(void* address);

std::optional<std::uintptr_t> ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count);

std::string ProtectionName(DWORD protection);
std::string StateName(DWORD state);
std::string TypeName(DWORD type);

/* Typed read.  `src/mem/il2cpp.cpp` calls `cabbird::mem::Read<std::int32_t>(address)` and
 * `Read<std::uintptr_t>(address)`, so the template belongs in the header, implemented over
 * the byte-level `ReadMemoryInto` that src/mem/memory.cpp defines.  Reconstructed along with
 * the rest of this file -- see the header comment. */
template <typename T>
[[nodiscard]] std::optional<T> Read(std::uintptr_t address) {
    T value{};
    if (!ReadMemoryInto(address, &value, sizeof(T))) {
        return std::nullopt;
    }
    return value;
}

template <typename T>
[[nodiscard]] std::optional<T> ReadAt(std::uintptr_t base, std::ptrdiff_t offset) {
    const auto address = static_cast<std::uintptr_t>(
        static_cast<std::ptrdiff_t>(base) + offset);
    return Read<T>(address);
}

template <typename T>
[[nodiscard]] bool Write(std::uintptr_t address, const T& value) {
    return WriteMemory(address, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] bool Patch(std::uintptr_t address, const T& value) {
    return PatchMemory(address, &value, sizeof(T));
}

/* Walk `count` pointer dereferences, adding one offset after each, then read T there.
 * Matches ResolvePointerChain's arithmetic exactly: current = *(current) + offsets[i].
 * The two implementations must stay in step -- they are the same walk written twice. */
template <typename T>
[[nodiscard]] std::optional<T> ReadChainValue(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) {
    const auto resolved = ResolvePointerChain(base, offsets, count);
    if (!resolved) {
        return std::nullopt;
    }
    return Read<T>(*resolved);
}

}  // namespace cabbird::mem
