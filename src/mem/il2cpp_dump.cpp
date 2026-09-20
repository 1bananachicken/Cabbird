#include "cabbird/il2cpp_dump.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "cabbird/il2cpp.hpp"
#include "cabbird/memory.hpp"

namespace cabbird::il2cpp {
namespace {

// --- fault containment -----------------------------------------------------------
//
// A dumper living inside someone else's process does not get to kill it.  That is
// the lesson from an in-game run that died with
//
//     0xC0000005 (ACCESS_VIOLATION, READ) in GameAssembly.dll + 0x13E15C2
//     accessing 0x0000000000000135
//     on the probe thread, during the dump
//
// 0x135 is a null base plus the offset of Il2CppClass::instance_size/actualSize,
// so a class pointer the runtime had not finished building was dereferenced.  The
// fields-only path had completed cleanly on the same game, and the only new
// per-class calls were the two that lazily INITIALISE a class
// (Class::SetupProperties / Class::SetupInterfaces).
//
// Two things follow, and both are implemented here and in the caller:
//
//   1. Those initialising walks are opt-in (see DumpOptions), and the thread is
//      attached to the IL2CPP domain first (DumpOptions::attach_thread).
//   2. Every call into the runtime that can fault is wrapped, so the worst case is
//      a skipped class rather than a dead game.  A swallowed fault is COUNTED and
//      reported in DumpStats, because a silently missing class is the one outcome
//      worse than a visible failure.
//
// MSVC will not accept __try in a function that needs C++ object unwinding
// (C2712), so each guard is a small free function whose only locals are PODs.
// Only access violations are swallowed; a stack overflow or an illegal
// instruction keeps propagating, because this dumper has no business hiding those.

std::atomic<std::size_t> g_contained_faults{0};

// EXCEPTION_CONTINUE_SEARCH for anything that is not an access violation.
#define CABBIRD_SEH_FILTER                                                             \
    (::GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER     \
                                                        : EXCEPTION_CONTINUE_SEARCH)

#define CABBIRD_SEH_NOTE() (void)++g_contained_faults

const MethodInfo* GuardedGetMethods(const Api& api, Il2CppClass* klass, void** iterator) {
    __try {
        return api.il2cpp_class_get_methods(klass, iterator);
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return nullptr;
    }
}

const PropertyInfo* GuardedGetProperties(const Api& api, Il2CppClass* klass, void** iterator) {
    __try {
        return api.il2cpp_class_get_properties(klass, iterator);
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return nullptr;
    }
}

Il2CppClass* GuardedGetInterfaces(const Api& api, Il2CppClass* klass, void** iterator) {
    __try {
        return api.il2cpp_class_get_interfaces(klass, iterator);
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return nullptr;
    }
}

const char* GuardedPropertyName(const Api& api, const PropertyInfo* property) {
    __try {
        return api.il2cpp_property_get_name(property);
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return nullptr;
    }
}

const MethodInfo* GuardedPropertyAccessor(const Api& api, const PropertyInfo* property, bool getter) {
    __try {
        return getter ? api.il2cpp_property_get_get_method(property)
                      : api.il2cpp_property_get_set_method(property);
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return nullptr;
    }
}

bool GuardedFieldFlags(const Api& api, FieldInfo* field, std::uint32_t& out) {
    __try {
        out = api.il2cpp_field_get_flags(field);
        return true;
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return false;
    }
}

bool GuardedMethodToken(const Api& api, const MethodInfo* method, std::uint32_t& out) {
    __try {
        out = api.il2cpp_method_get_token(method);
        return true;
    } __except (CABBIRD_SEH_FILTER) {
        CABBIRD_SEH_NOTE();
        return false;
    }
}

// The one word that identifies a plaintext metadata blob.  On disk this target
// has it zero times; in memory the runtime has to produce it to run at all.
constexpr std::uint32_t kMetadataSanity = 0xFAB11BAF;

// What IL2CPP returns from il2cpp_field_get_offset for a [ThreadStatic] field.
// It is a SENTINEL, not an offset: (size_t)-1.  Compared against the conversion
// of -1 rather than written as 0xFFFFFFFFFFFFFFFF so the meaning survives a
// change of width.
constexpr std::size_t kThreadStaticOffset = static_cast<std::size_t>(-1);

// Published metadata versions run from the Unity 4 era to the present.  Anything
// outside this window is a coincidence, not a header.
constexpr std::int32_t kMinVersion = 16;
constexpr std::int32_t kMaxVersion = 32;

// Version 29 (Unity 2022.3) has well under 96 fields; the cap only exists so a
// malformed header cannot make the parse run away.
constexpr std::size_t kMaxHeaderPairs = 128;

// How many plausible pairs a candidate must show before it is believed.
//
// This is the real discriminator, and the version field is not.  A random
// occurrence of the 4-byte magic is followed by random bytes, so its pairs are
// almost immediately implausible -- typically 0 or 1 survive.  A genuine header
// has dozens.  A threshold of 8 sits far from both, so it accepts a real header
// whose version field we do not recognise (which is exactly the case a
// version-only check wrongly rejects) while still rejecting coincidences.
constexpr std::size_t kMinSanePairs = 8;

// Reported candidates are capped so a pathological run cannot flood the log.
constexpr std::size_t kMaxReportedCandidates = 16;

// A 48 MiB blob was measured on disk for this target, so anything past this is a
// false positive rather than a real blob.
constexpr std::size_t kMaxBlobSize = 512u << 20;
constexpr std::size_t kMinBlobSize = 64u << 10;

// Scan window.  Small enough that a multi-gigabyte heap is never copied whole,
// large enough that the per-window overhead disappears.
constexpr std::size_t kScanWindow = 8u << 20;
constexpr std::size_t kScanOverlap = 3;  // so the magic is found across a boundary

constexpr std::size_t kHeaderProbeSize = 0x400;

// Hex text for one value.
//
// RETURNS the text rather than appending to an output parameter, and that signature is
// the fix for a real defect rather than a style preference: this used to be
// `AppendHex(std::string& out, ...)`, and a bulk rewrite of the dump body from one buffer
// to another changed the ARGUMENT at five call sites while leaving the function alone, so
// every field offset and every method RVA/VA/Token was appended to a temporary string that
// was discarded when the dump returned.  Compilers and linkers were
// silent -- the dump simply had 262,816 field lines ending in "; // " and no method lines at
// all, with a header that truthfully said methods=1.  With a return value there is no
// second buffer to confuse it with.
std::string HexText(std::size_t value) {
    char buffer[32]{};
    // A single conversion for both callers: the provenance header passes uintptr_t and the
    // walk passes size_t, and on this target those are the same width.  Printing through
    // unsigned long long with an explicit cast keeps the format string honest on x64 and
    // would keep it honest on a 32-bit build too.
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

// Whether methods should be dumped for an image with this name.
//
// An empty filter means "every image", which is the old behaviour; a non-empty
// one is a substring test, so {"Azur"} covers all five of the target's own
// assemblies without needing a per-image list that a patch would invalidate.
bool MethodImageMatches(const std::string& image_name,
                        const std::vector<std::string>& filter) {
    if (filter.empty()) {
        return true;
    }
    for (const std::string& needle : filter) {
        if (!needle.empty() && image_name.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// IL2CPP field/method attribute bits, mirrored from the runtime's own header so
// a reader can tell a static field from an instance one.
constexpr std::uint32_t kMethodAttributeStatic = 0x0010;
constexpr std::uint32_t kMethodAttributePublic = 0x0006;
constexpr std::uint32_t kMethodAttributeAccessMask = 0x0007;

// FieldAttributes, same values as the CLR's.  Only the ones that change how a
// reader interprets a field are used.
constexpr std::uint32_t kFieldAttributeAccessMask = 0x0007;
constexpr std::uint32_t kFieldAttributePublic = 0x0006;
constexpr std::uint32_t kFieldAttributeStatic = 0x0010;
constexpr std::uint32_t kFieldAttributeInitOnly = 0x0020;  // readonly
constexpr std::uint32_t kFieldAttributeLiteral = 0x0040;   // const
constexpr std::uint32_t kFieldAttributeNotSerialized = 0x0080;

// Guard against a malformed interface chain turning into a pathological loop.
// A class implementing more than a few dozen interfaces is not real code, and an
// unbounded walk against a corrupt iterator would hang the render thread.
constexpr std::size_t kMaxInterfacesPerClass = 64;

// Same reasoning for properties: an unbounded walk against a corrupt iterator
// would hang the render thread rather than fail visibly.
constexpr std::size_t kMaxPropertiesPerClass = 512;

// Renders the access modifier from FieldAttributes' 3-bit access mask.
//
// All seven values are covered deliberately.  An earlier version handled only
// private/protected/public and fell through to nothing for the rest, which meant
// a `FamANDAssem` field rendered with NO modifier at all -- reading as if it were
// public.  Claiming more visibility than a field really has is the one direction
// this must never fail in, so the fallback is `internal`, never "public" and
// never "nothing".
std::string AccessModifier(std::uint32_t access) {
    switch (access & 0x0007) {
        case 0x0001: return "private ";
        case 0x0002: return "private protected ";
        case 0x0003: return "internal ";
        case 0x0004: return "protected ";
        case 0x0005: return "protected internal ";
        case 0x0006: return "public ";
        default: return "internal ";
    }
}

std::string FieldModifiers(std::uint32_t flags) {
    std::string out = AccessModifier(flags);
    if ((flags & kFieldAttributeLiteral) != 0) {
        // C# has no `static const`: a const is implicitly static, and both
        // Il2CppDumper and Roslyn print it that way.
        out += "const ";
    } else if ((flags & kFieldAttributeStatic) != 0) {
        out += "static ";
        if ((flags & kFieldAttributeInitOnly) != 0) {
            out += "readonly ";
        }
    } else if ((flags & kFieldAttributeInitOnly) != 0) {
        out += "readonly ";
    }
    return out;
}

// Same idea for methods.
std::string MethodModifiers(std::uint32_t flags) {
    std::string out = AccessModifier(flags);
    if ((flags & kMethodAttributeStatic) != 0) {
        out += "static ";
    }
    if ((flags & 0x0400) != 0) {  // Abstract
        out += "abstract ";
    }
    if ((flags & 0x0040) != 0) {  // Virtual
        out += "virtual ";
    }
    return out;
}

// Reads `MethodInfo::methodPointer`, which the runtime does not export a getter
// for, and reports whether the result actually landed inside the module.
//
// The offset is 0 because Unity declares methodPointer as the first member.  That
// declaration comes from a Unity version other than the one this target runs, so
// the range check is what makes the value trustworthy rather than merely
// plausible: code pointers live in the module's executable range, and garbage
// almost never does.
bool ReadMethodPointer(const MethodInfo* method, std::uintptr_t module_base,
                       std::uintptr_t module_size, std::uintptr_t& out) {
    out = 0;
    if (method == nullptr) {
        return false;
    }
    std::uintptr_t pointer = 0;
    // ReadMemory returns a fresh vector (it has a 4096-byte cap, which an 8-byte
    // read is nowhere near); the pointer is copied out of it before it dies.
    const auto bytes = mem::ReadMemory(reinterpret_cast<std::uintptr_t>(method), sizeof(pointer));
    if (!bytes.has_value() || bytes->size() != sizeof(pointer)) {
        return false;
    }
    std::memcpy(&pointer, bytes->data(), sizeof(pointer));
    if (pointer == 0) {
        return false;
    }
    if (module_base == 0 || module_size == 0) {
        out = pointer;
        return true;  // no range to check against
    }
    if (pointer < module_base || pointer >= module_base + module_size) {
        return false;
    }
    out = pointer;
    return true;
}

std::string QualifyClass(Il2CppClass* klass) {
    const Api& api = Functions();
    const char* name = api.il2cpp_class_get_name != nullptr
                           ? api.il2cpp_class_get_name(klass)
                           : nullptr;
    std::string result;
    if (api.il2cpp_class_get_namespace != nullptr) {
        const char* space = api.il2cpp_class_get_namespace(klass);
        if (space != nullptr && *space != '\0') {
            result = space;
            result += ".";
        }
    }
    result += name != nullptr ? name : "?";
    return result;
}

bool IsReadableProtection(DWORD protection) {
    // PAGE_GUARD and PAGE_NOACCESS first: a guarded page is "readable" by mask
    // but touching it raises STATUS_GUARD_PAGE_VIOLATION, and the whole point of
    // this scan is to not take the process down.
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// Copies a large object out of the address space, in chunks.
//
// Deliberately NOT mem::ReadMemory: that one refuses anything over 4 KiB, a guard
// that exists so a stray pointer cannot make the process copy hundreds of
// megabytes.  The guard is right, but it returns the same nullopt for "too big"
// as for "unreadable", so using it for a legitimate 48 MiB blob fails silently
// and looks exactly like the blob not being there.  That is a mistake this code
// already made once, and the magic-candidate counters above are what caught it.
//
// Chunking also keeps a single ReadProcessMemory from spanning a region whose
// protection changes part-way, which would fail the whole copy.
bool ReadBlobChunked(std::uintptr_t address, std::size_t size, std::vector<std::uint8_t>& out) {
    constexpr std::size_t kChunk = 1u << 20;
    out.clear();
    out.resize(size);
    for (std::size_t offset = 0; offset < size; offset += kChunk) {
        const std::size_t want = std::min(kChunk, size - offset);
        if (!mem::ReadMemoryInto(address + offset, out.data() + offset, want)) {
            out.clear();
            return false;
        }
    }
    return true;
}

// --- the process-wide metadata cache ---------------------------------------------
//
// One blob per process, replaced only by ResetMetadataCache().  `metadata_mutex`
// serialises two callers that miss the cache at the same time: without it both would
// run the 6.6 s scan AND both would allocate 50 MB, which on a game process with a
// few GB of headroom is a real cost for a question that has one answer.
//
// The returned pointer stays valid because only ResetMetadataCache() frees the blob,
// and it is documented as the offline test's tool.
std::mutex g_metadata_mutex;
std::unique_ptr<MetadataBlob> g_metadata_blob;

// The dump's own provenance line.
//
// WHY THE HEADER CARRIES THIS AND NOT A SIDECAR FILE: the counters that decide whether
// a dump can be trusted -- how many method pointers landed inside the module, how many
// runtime calls faulted, which module range "inside" even means -- would otherwise live in
// a status file and the log, i.e. in two places a reader of dump.cs does not
// have.  A dump that cannot state its own coverage gets read as complete.  Every number
// here is one DumpStats already measures; nothing new is collected, only published
// where the reader is.
void AppendProvenanceHeader(std::string& out, const DumpStats& stats, const DumpOptions& options) {
    out += "// module: GameAssembly.dll base=";
    out += options.module_base != 0 ? HexText(options.module_base) : std::string("(unset)");
    out += " size=" + HexText(options.module_size);
    if (options.module_base == 0) {
        // Stated rather than implied: with no base the address block on every method is
        // a raw VA, which changes with ASLR and cannot be filed against the file.
        out += " [RVA NOT DERIVED: the address block below holds VA values]";
    }
    out += "\n";

    out += "// method pointers: inside_module=" + std::to_string(stats.method_pointers_inside) +
           " outside_module=" + std::to_string(stats.method_pointers_outside) + "\n";
    out += "// counters: images=" + std::to_string(stats.images) +
           " classes=" + std::to_string(stats.classes) +
           " fields=" + std::to_string(stats.fields) +
           " methods=" + std::to_string(stats.methods) +
           " properties=" + std::to_string(stats.properties) +
           " contained_faults=" + std::to_string(stats.contained_faults) +
           " thread_attached=" + std::string(stats.thread_attached ? "1" : "0") + "\n";
    // Why this is load-bearing enough to print on every dump: properties and interfaces
    // were the walks that crashed a game, and methods are the switch whose ABSENCE
    // produced 45,578 method lines with none of them from the game's own main assembly.
    // "which walks were on for this file" is the first question asked of a dump that
    // looks thin, and it must not need a second artifact to answer.
    out += "// walks requested: fields=" + std::string(options.include_fields ? "1" : "0") +
           " methods=" + std::string(options.include_methods ? "1" : "0") +
           " properties=" + std::string(options.include_properties ? "1" : "0") +
           " interfaces=" + std::string(options.include_interfaces ? "1" : "0") +
           " method_image_filter=" +
           (options.method_image_filter.empty()
                ? std::string("(all images)")
                : [&options] {
                      std::string joined;
                      for (const std::string& needle : options.method_image_filter) {
                          if (!joined.empty()) joined += ",";
                          joined += needle;
                      }
                      return joined;
                  }()) +
           "\n";
    if (stats.metadata_size != 0) {
        out += "// metadata: recovered plaintext blob, size=" + std::to_string(stats.metadata_size) +
               " version=" + std::to_string(stats.metadata_version) +
               " address=" + HexText(stats.metadata_address) +
               " scan_ms=" + std::to_string(stats.metadata_scan_ms) +
               " written=" + std::string(stats.metadata_written ? "1" : "0") + "\n";
    } else if (options.recover_metadata) {
        // Recovery was asked for and produced nothing.  Saying so is the difference
        // between "the metadata is not here" and "nobody asked", which a missing line
        // would leave indistinguishable.
        out += "// metadata: recovery requested but no plaintext blob was found\n";
    }
}

}  // namespace

MetadataHeaderScore ScoreMetadataHeader(const std::uint8_t* header, std::size_t available) {
    MetadataHeaderScore score{};
    if (header == nullptr || available < 8 + 8) {
        return score;
    }
    const std::size_t pairs = std::min(kMaxHeaderPairs, (available - 8) / 8);
    for (std::size_t index = 0; index < pairs; ++index) {
        std::int32_t offset = 0;
        std::int32_t size = 0;
        std::memcpy(&offset, header + 8 + index * 8, sizeof(offset));
        std::memcpy(&size, header + 8 + index * 8 + 4, sizeof(size));

        if (offset == 0 && size == 0) {
            ++score.sane_pairs;  // an unused table is normal
            continue;
        }
        // Past the real header sits table data.  Read as a pair that is either a
        // negative size or an offset beyond any plausible blob -- so stop rather
        // than let a garbage value define the size.
        if (offset < 0 || size < 0) {
            break;
        }
        const std::size_t candidate =
            static_cast<std::size_t>(offset) + static_cast<std::size_t>(size);
        if (candidate > kMaxBlobSize) {
            break;
        }
        ++score.sane_pairs;
        score.derived_size = std::max(score.derived_size, candidate);
    }
    return score;
}

std::size_t SizeMetadataFromHeader(const std::uint8_t* header, std::size_t available) {
    return ScoreMetadataHeader(header, available).derived_size;
}

std::optional<MetadataBlob> FindMetadataBlob(std::string& error, std::string* diagnostics) {
    error.clear();
    if (diagnostics != nullptr) {
        diagnostics->clear();
    }

    std::size_t regions_seen = 0;
    std::size_t regions_skipped = 0;
    std::size_t regions_scanned = 0;
    std::size_t bytes_scanned = 0;
    std::size_t magic_candidates = 0;

    // Every candidate that matched the magic, accepted or not.  Reported either
    // way: a run that fails has to say what it saw, not just that it found
    // nothing, or the next iteration has no more information than this one did.
    struct Candidate {
        std::uintptr_t address{};
        DWORD region_type{};
        std::int32_t version{};
        MetadataHeaderScore score{};
    };
    std::vector<Candidate> candidates;

    std::vector<std::uint8_t> window(kScanWindow + kScanOverlap);

    // Three passes, covering every committed region type.
    //
    // The first version scanned MEM_PRIVATE only, on the reasoning that IL2CPP
    // mallocs the metadata buffer.  That reports "no blob" after covering a few
    // hundred private regions -- and that answer is close to
    // worthless, because a private-only scan cannot tell "the metadata is not in
    // memory" apart from "the metadata is in memory, just not in a private
    // region".  Both produce the identical empty result.
    //
    // So the passes now cover everything committed.  The cost is scanning mapped
    // views and PE images on top of the heap, which is a few seconds on a
    // background thread -- a fair price for an answer that actually means
    // something.
    const DWORD pass_types[] = {MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE};
    for (const DWORD wanted_type : pass_types) {
        for (const mem::RegionInfo& region : mem::EnumerateProcessRegions()) {
            ++regions_seen;
            if (region.type != wanted_type || region.state != MEM_COMMIT ||
                !IsReadableProtection(region.protection)) {
                ++regions_skipped;
                continue;
            }
            ++regions_scanned;

            for (std::size_t offset = 0; offset < region.size; offset += kScanWindow) {
                const std::size_t remaining = region.size - offset;
                const std::size_t want = std::min(kScanWindow + kScanOverlap, remaining);
                if (!mem::ReadMemoryInto(region.base + offset, window.data(), want)) {
                    // One unreadable window must not abandon the search; a region
                    // can be readable in the middle and not at the edges.
                    continue;
                }
                bytes_scanned += want;

                for (std::size_t index = 0; index + 4 <= want; ++index) {
                    if (window[index] != 0xAF || window[index + 1] != 0x1B ||
                        window[index + 2] != 0xB1 || window[index + 3] != 0xFA) {
                        continue;
                    }
                    ++magic_candidates;
                    const std::uintptr_t candidate_address = region.base + offset + index;

                    std::uint8_t header[kHeaderProbeSize]{};
                    const std::size_t header_available =
                        std::min<std::size_t>(kHeaderProbeSize, region.size - (offset + index));
                    if (header_available < 16 ||
                        !mem::ReadMemoryInto(candidate_address, header, header_available)) {
                        continue;
                    }
                    std::int32_t sanity = 0;
                    std::int32_t version = 0;
                    std::memcpy(&sanity, header, sizeof(sanity));
                    std::memcpy(&version, header + 4, sizeof(version));
                    if (sanity != static_cast<std::int32_t>(kMetadataSanity)) {
                        continue;  // the bytes merely looked like the magic
                    }

                    Candidate found;
                    found.address = candidate_address;
                    found.region_type = region.type;
                    found.version = version;
                    found.score = ScoreMetadataHeader(header, header_available);
                    candidates.push_back(found);
                }
            }
        }
    }

    // Choose the best candidate.  Preference: a recognised version AND a
    // believable structure; then believable structure alone, because a real
    // header with an unexpected version is far more likely than a coincidence
    // that happens to have dozens of sane table pairs.
    const Candidate* best = nullptr;
    bool best_version_recognised = false;
    for (const Candidate& candidate : candidates) {
        if (candidate.score.sane_pairs < kMinSanePairs) {
            continue;
        }
        const bool recognised = candidate.version >= kMinVersion && candidate.version <= kMaxVersion;
        const bool best_recognised =
            best != nullptr && best_version_recognised;
        if (best == nullptr || (recognised && !best_recognised)) {
            best = &candidate;
            best_version_recognised = recognised;
        }
    }

    // Report every candidate, always.  This is the difference between "not found"
    // and "here is everything that looked like it".
    if (diagnostics != nullptr) {
        char line[256]{};
        // The pass count is computed, not typed.  The previous hardcoded
        // "(2 passes)" survived the change to three passes and would have told a
        // future reader that mapped regions were not scanned on a run where they
        // were -- a log that lies about what was covered is worse than no log.
        std::snprintf(line, sizeof(line),
                      "%zu magic candidates; %zu MiB scanned over %zu regions (%zu passes)\n",
                      magic_candidates, bytes_scanned >> 20, regions_scanned,
                      sizeof(pass_types) / sizeof(pass_types[0]));
        *diagnostics += line;
        for (const Candidate& candidate : candidates) {
            std::snprintf(line, sizeof(line),
                          "  candidate at %p type=0x%lX version=%-6d sane_pairs=%-4zu "
                          "derived_size=%-10zu %s\n",
                          reinterpret_cast<void*>(candidate.address),
                          static_cast<unsigned long>(candidate.region_type), candidate.version,
                          candidate.score.sane_pairs, candidate.score.derived_size,
                          candidate.score.sane_pairs >= kMinSanePairs
                              ? (candidate.version >= kMinVersion && candidate.version <= kMaxVersion
                                     ? "<-- ACCEPTED"
                                     : "<-- would be accepted, but the version is unexpected")
                              : "rejected (structure does not look like a metadata header)");
            *diagnostics += line;
            if (diagnostics->size() > 8192) {
                *diagnostics += "  ... candidate list truncated\n";
                break;
            }
        }
    }

    if (best != nullptr) {
        MetadataBlob blob;
        blob.address = best->address;
        blob.size = best->score.derived_size;
        blob.version = best->version;
        blob.region_type = best->region_type;
        if (best->score.derived_size >= kMinBlobSize &&
            best->score.derived_size <= kMaxBlobSize &&
            ReadBlobChunked(best->address, best->score.derived_size, blob.bytes)) {
            return blob;
        }
        error = "a candidate scored high enough to believe but could not be copied out";
        return std::nullopt;
    }

    char detail[512]{};
    std::snprintf(detail, sizeof(detail),
                  "no plaintext metadata blob: %zu region visits, %zu not private/mapped+readable "
                  "(%zu scanned), %zu MiB scanned, %zu magic candidates, none with >= %zu sane "
                  "header pairs",
                  regions_seen, regions_skipped, regions_scanned, bytes_scanned >> 20,
                  magic_candidates, kMinSanePairs);
    error = detail;
    return std::nullopt;
}

const MetadataBlob* CachedMetadataBlob(std::string& error, std::uint64_t* elapsed_ms) {
    if (elapsed_ms != nullptr) *elapsed_ms = 0;

    // Fast path: the lock is taken even here, because the alternative is a data race on
    // the unique_ptr during the one moment ResetMetadataCache() runs.  The cost is an
    // uncontended mutex per call, against a 50 MB scan that happens once.
    std::scoped_lock lock(g_metadata_mutex);
    if (g_metadata_blob != nullptr) return g_metadata_blob.get();

    const std::uint64_t started = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::string scan_error;
    std::optional<MetadataBlob> found = FindMetadataBlob(scan_error, nullptr);
    if (!found) {
        error = scan_error;
        return nullptr;
    }
    g_metadata_blob = std::make_unique<MetadataBlob>(std::move(*found));
    if (elapsed_ms != nullptr) {
        const std::uint64_t finished = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        *elapsed_ms = finished > started ? finished - started : 0;
    }
    return g_metadata_blob.get();
}

void ResetMetadataCache() {
    std::scoped_lock lock(g_metadata_mutex);
    g_metadata_blob.reset();
}

bool WriteMetadataBlob(const MetadataBlob& blob, const std::wstring& path, std::string& error) {
    if (path.empty()) {
        error = "no path was given";
        return false;
    }
    if (blob.bytes.empty()) {
        error = "the blob is empty";
        return false;
    }
    // Truncation matters here in a way it does not for the text dump: a stale larger
    // file left behind by an earlier run would still expose its tail, so a reader
    // taking `size` bytes and a reader taking the whole file would disagree about what
    // the metadata is.  `trunc` is what stops that.
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "output stream could not be opened";
        return false;
    }
    output.write(reinterpret_cast<const char*>(blob.bytes.data()),
                 static_cast<std::streamsize>(blob.bytes.size()));
    if (!output) {
        error = "write failed";
        return false;
    }
    return true;
}

DumpStats DumpTypeSystem(std::string& out, const DumpOptions& options) {
    DumpStats stats{};
    const Api& api = Functions();

    // Publish progress from inside the walk.  `report` is a no-op when the caller asked
    // for no callback, so every other caller's behaviour is unchanged.
    //
    // TIME IS THE PRIMARY TRIGGER, not the class count.  A class-count-only schedule
    // reported nothing for seconds at a time on the first real-machine run (an image with
    // `include_methods` can hold few classes but enormous method counts), which made the
    // host's own liveness display cry wolf.  `steady_clock::now()` once per class is a few
    // nanoseconds against the ~8 interop calls each method costs, so this is free by
    // comparison.
    std::uint64_t last_report_ms = 0;
    // Deliberately NOT a nested lambda: capturing a lambda inside another lambda made the
    // inner call ambiguous to the compiler (C3861/C2440), and the walk does not need the
    // indirection -- `steady_clock::now()` inline is one line and has no capture problems.
    const auto report = [&options, &stats, &last_report_ms](bool force) noexcept {
        if (options.progress == nullptr) return;
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (!force) {
            const bool time_due = options.progress_interval_ms != 0 &&
                now - last_report_ms >= options.progress_interval_ms;
            const bool count_due = options.progress_class_interval != 0 &&
                (stats.classes & (options.progress_class_interval - 1)) == 0;
            if (!time_due && !count_due) return;
        }
        last_report_ms = now;
        options.progress(options.progress_user, stats.images, stats.classes, stats.fields);
    };

    // Attach before the first runtime call, not merely before the initialising
    // ones: `il2cpp_domain_get` itself is only meaningful for a thread the
    // runtime can describe.  ThreadScope is reference-counted and leaves a
    // thread the runtime already knows alone, so this is safe to enter even on
    // Unity's own main thread.
    std::unique_ptr<ThreadScope> thread_scope;
    if (options.attach_thread) {
        thread_scope = std::make_unique<ThreadScope>();
        stats.thread_attached = thread_scope->attached();
    }

    // Clear the counter so a caller reading stats.contained_faults sees exactly
    // the faults from THIS dump rather than a running total.
    g_contained_faults.store(0);

    // The metadata recovery runs BEFORE the walk, and it does not need the walk's
    // results.  Two reasons for this order rather than "after":
    //
    //   * A walk can be long (measured: 891 ms for fields only, tens of seconds with
    //     methods across every image), and the blob is the input the file parser needs.
    //     Reporting the recoverable half first means a run that is killed mid-walk
    //     still leaves the metadata on disk.
    //   * A fault in the metadata scan cannot be confused with a fault in the walk:
    //     `contained_faults` is reset above and the scan takes no guarded runtime
    //     calls, so a non-zero counter afterwards belongs to the walk alone.
    if (options.recover_metadata) {
        std::string metadata_error;
        const MetadataBlob* blob = CachedMetadataBlob(metadata_error, &stats.metadata_scan_ms);
        if (blob != nullptr) {
            stats.metadata_size = blob->size;
            stats.metadata_version = blob->version;
            stats.metadata_address = blob->address;
            if (!options.metadata_path.empty()) {
                std::string write_error;
                stats.metadata_written = WriteMetadataBlob(*blob, options.metadata_path, write_error);
            }
        }
    }

    Il2CppDomain* domain = api.il2cpp_domain_get != nullptr ? api.il2cpp_domain_get() : nullptr;
    if (domain == nullptr) {
        out += "// no IL2CPP domain\n";
        return stats;
    }

    std::size_t assembly_count = 0;
    const Il2CppAssembly** assemblies =
        api.il2cpp_domain_get_assemblies != nullptr
            ? api.il2cpp_domain_get_assemblies(domain, &assembly_count)
            : nullptr;
    if (assemblies == nullptr) {
        out += "// no assemblies\n";
        return stats;
    }

    // The body is built separately from the header.
    //
    // It used to be appended to `out` directly, with a three-line title at the top.  That
    // makes the provenance counters -- how many method pointers landed inside the module,
    // how many runtime calls faulted -- impossible to write, because they are only known
    // AFTER the walk, and a file whose header says "inside=0" before the walk has run is
    // worse than one that says nothing.  So the body accumulates here and the header is
    // composed from the finished stats at the end.
    std::string body;

    for (std::size_t assembly_index = 0; assembly_index < assembly_count; ++assembly_index) {
        if (assemblies[assembly_index] == nullptr ||
            api.il2cpp_assembly_get_image == nullptr) {
            continue;
        }
        Il2CppImage* image = api.il2cpp_assembly_get_image(assemblies[assembly_index]);
        if (image == nullptr) {
            continue;
        }
        const char* image_name =
            api.il2cpp_image_get_name != nullptr ? api.il2cpp_image_get_name(image) : nullptr;
        const std::size_t class_count =
            api.il2cpp_image_get_class_count != nullptr
                ? api.il2cpp_image_get_class_count(image)
                : 0;
        if (class_count == 0) {
            continue;  // nothing to say about an empty image
        }
        ++stats.images;

        body += "// ===== image: ";
        body += image_name != nullptr ? image_name : "?";
        body += " (";
        body += std::to_string(class_count);
        body += " classes)\n\n";

        const std::size_t limit =
            options.max_classes_per_image == 0
                ? class_count
                : std::min(class_count, options.max_classes_per_image);
        if (limit < class_count) {
            stats.truncated_classes += class_count - limit;
        }

        for (std::size_t class_index = 0; class_index < limit; ++class_index) {
            Il2CppClass* klass =
                api.il2cpp_image_get_class != nullptr ? api.il2cpp_image_get_class(image, class_index)
                                                      : nullptr;
            if (klass == nullptr) {
                continue;
            }
            ++stats.classes;

            // Value types are `struct` in C#, and Il2CppDumper writes them that way.
            // This dumper used to call every one of them a `class` and lean on the
            // `: System.ValueType` base to convey the truth -- which read as a
            // contradiction rather than as information.
            //
            // Found by diffing this output against Il2CppDumper's: `public struct
            // AzurBrick` there, `public class AzurBrick : System.ValueType` here.
            // Both files were "correct"; only one was readable.
            const bool is_value_type =
                api.il2cpp_class_is_valuetype != nullptr && api.il2cpp_class_is_valuetype(klass);
            body += is_value_type ? "public struct " : "public class ";
            body += QualifyClass(klass);

            Il2CppClass* parent =
                api.il2cpp_class_get_parent != nullptr ? api.il2cpp_class_get_parent(klass) : nullptr;
            if (parent != nullptr) {
                body += " : ";
                body += QualifyClass(parent);
            }

            // Interfaces are emitted on the same `:` line as the base class, the
            // way C# and Il2CppDumper both write them.  Without them a reader
            // cannot tell which contract a class actually implements.
            //
            // Guarded and opt-in: see the fault-containment block at the top of
            // this file for why this specific call is the one that killed a game.
            if (options.include_interfaces && api.il2cpp_class_get_interfaces != nullptr) {
                void* iterator = nullptr;
                bool first = parent == nullptr;
                std::size_t emitted = 0;
                while (Il2CppClass* iface = GuardedGetInterfaces(api, klass, &iterator)) {
                    if (emitted >= kMaxInterfacesPerClass) {
                        body += ", /* ... */";
                        break;
                    }
                    body += first ? " : " : ", ";
                    first = false;
                    body += QualifyClass(iface);
                    ++emitted;
                }
            }
            body += "\n{\n";

            if (options.include_fields && api.il2cpp_class_get_fields != nullptr) {
                void* iterator = nullptr;
                std::size_t field_count = 0;
                while (FieldInfo* field = api.il2cpp_class_get_fields(klass, &iterator)) {
                    if (options.max_fields_per_class != 0 &&
                        field_count >= options.max_fields_per_class) {
                        body += "    // ... more fields elided\n";
                        break;
                    }
                    const char* field_name =
                        api.il2cpp_field_get_name != nullptr ? api.il2cpp_field_get_name(field)
                                                             : nullptr;
                    const std::size_t offset =
                        api.il2cpp_field_get_offset != nullptr
                            ? api.il2cpp_field_get_offset(field)
                            : 0;
                    // Guarded: a per-field call is the highest-frequency runtime
                    // call this dumper makes (101,231 of them on this game), so it
                    // is also the one with the most chances to meet a bad pointer.
                    std::uint32_t field_flags = 0;
                    if (api.il2cpp_field_get_flags != nullptr) {
                        GuardedFieldFlags(api, field, field_flags);
                    }
                    body += "    ";
                    body += FieldModifiers(field_flags);
                    if (api.il2cpp_field_get_type != nullptr && api.il2cpp_type_get_name != nullptr) {
                        const Il2CppType* type = api.il2cpp_field_get_type(field);
                        const char* type_name = type != nullptr ? api.il2cpp_type_get_name(type) : nullptr;
                        body += type_name != nullptr ? type_name : "?";
                    } else {
                        body += "?";
                    }
                    body += " ";
                    body += field_name != nullptr ? field_name : "?";
                    body += "; // ";
                    // THREAD-STATIC FIELDS.
                    //
                    // IL2CPP reports the offset of a [ThreadStatic] field as
                    // (size_t)-1, which printed verbatim is `0xFFFFFFFFFFFFFFFF` --
                    // a value that looks like an address and is not one.  It is a
                    // marker, and a useful one: it says the field has no fixed
                    // location at all and lives in per-thread storage, so a modder
                    // cannot reach it with an object pointer and an offset.
                    //
                    // Measured, not assumed: all 39 fields in this game carrying
                    // that value are `static`, and every one of them is [ThreadStatic]
                    // (`t_threadRandom`, `type_resolve_in_progress`, ...).  Il2CppDumper
                    // renders the same fields with its own sentinel rather than the
                    // raw number, and the comparison is what surfaced this.
                    const bool thread_static =
                        (field_flags & kFieldAttributeStatic) != 0 && offset == kThreadStaticOffset;
                    if (thread_static) {
                        body += "thread-static";
                    } else {
                        body += HexText(offset);
                    }
                    // Static and const fields carry no instance offset, so printing
                    // a bare `0x0` next to them invites the reader to treat it as a
                    // real answer.  Say so instead.  (For thread-static the line
                    // above already said something better, so only add this when it
                    // did not.)
                    if (!thread_static &&
                        (field_flags & (kFieldAttributeStatic | kFieldAttributeLiteral)) != 0) {
                        body += " (no instance offset)";
                    }
                    if ((field_flags & kFieldAttributeNotSerialized) != 0) {
                        body += " [NotSerialized]";
                    }
                    body += "\n";
                    ++stats.fields;
                    ++field_count;
                }
            }

            // nullptr-safe: image_name can be null, and constructing a std::string
            // from a null char* is undefined behaviour.
            const std::string image_name_string = image_name != nullptr ? image_name : "";
            if (options.include_methods && MethodImageMatches(image_name_string, options.method_image_filter) &&
                api.il2cpp_class_get_methods != nullptr) {
                void* iterator = nullptr;
                std::size_t method_count = 0;
                while (const MethodInfo* method = GuardedGetMethods(api, klass, &iterator)) {
                    if (options.max_methods_per_class != 0 &&
                        method_count >= options.max_methods_per_class) {
                        body += "    // ... more methods elided\n";
                        break;
                    }
                    const char* method_name =
                        api.il2cpp_method_get_name != nullptr ? api.il2cpp_method_get_name(method)
                                                              : nullptr;
                    body += "    ";
                    if (api.il2cpp_method_get_flags != nullptr) {
                        std::uint32_t iflags = 0;
                        const std::uint32_t flags = api.il2cpp_method_get_flags(method, &iflags);
                        body += MethodModifiers(flags);
                    } else {
                        body += "public ";
                    }
                    if (api.il2cpp_method_get_return_type != nullptr &&
                        api.il2cpp_type_get_name != nullptr) {
                        const Il2CppType* type = api.il2cpp_method_get_return_type(method);
                        const char* type_name = type != nullptr ? api.il2cpp_type_get_name(type) : nullptr;
                        body += type_name != nullptr ? type_name : "?";
                    } else {
                        body += "?";
                    }
                    body += " ";
                    body += method_name != nullptr ? method_name : "?";
                    body += "(";
                    const std::uint32_t parameter_count =
                        api.il2cpp_method_get_param_count != nullptr
                            ? api.il2cpp_method_get_param_count(method)
                            : 0;
                    for (std::uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
                        if (parameter != 0) {
                            body += ", ";
                        }
                        if (api.il2cpp_method_get_param != nullptr &&
                            api.il2cpp_type_get_name != nullptr) {
                            const Il2CppType* type = api.il2cpp_method_get_param(method, parameter);
                            // A generic parameter of an open generic method resolves
                            // to null; that is expected, not an error.
                            const char* type_name =
                                type != nullptr ? api.il2cpp_type_get_name(type) : nullptr;
                            body += type_name != nullptr ? type_name : "?";
                        } else {
                            body += "?";
                        }
                    }
                    body += ");";

                    // The address block is what makes a dump usable for RE rather
                    // than merely readable.  It is only printed when the pointer
                    // actually landed inside GameAssembly.dll -- see
                    // ReadMethodPointer for why it is measured and not assumed.
                    {
                        std::uintptr_t pointer = 0;
                        if (ReadMethodPointer(method, options.module_base, options.module_size,
                                              pointer)) {
                            ++stats.method_pointers_inside;
                            body += " // RVA: ";
                            body += HexText(options.module_base != 0 ? pointer - options.module_base
                                                                     : pointer);
                            body += " Offset: ";
                            body += HexText(options.module_base != 0 ? pointer - options.module_base
                                                                     : pointer);
                            body += " VA: ";
                            body += HexText(pointer);
                        } else {
                            ++stats.method_pointers_outside;
                            body += " // RVA: <not in module>";
                        }
                    }

                    if (api.il2cpp_method_get_token != nullptr) {
                        std::uint32_t method_token = 0;
                        if (GuardedMethodToken(api, method, method_token)) {
                            body += " Token: ";
                            body += HexText(method_token);
                        } else {
                            body += " Token: <unreadable>";
                        }
                    }
                    body += "\n";
                    ++stats.methods;
                    ++method_count;
                }
            }

            // Properties come after methods, matching Il2CppDumper's ordering so
            // the two dumps can be diffed by eye.
            //
            // Guarded and opt-in: il2cpp_class_get_properties runs
            // Class::SetupProperties internally, which WRITES to the class.  See
            // the fault-containment block at the top of this file.
            if (options.include_properties && api.il2cpp_class_get_properties != nullptr) {
                void* iterator = nullptr;
                std::size_t property_count = 0;
                while (const PropertyInfo* property =
                           GuardedGetProperties(api, klass, &iterator)) {
                    if (property_count >= kMaxPropertiesPerClass) {
                        body += "    // ... more properties elided\n";
                        break;
                    }
                    const char* property_name =
                        api.il2cpp_property_get_name != nullptr
                            ? GuardedPropertyName(api, property)
                            : nullptr;
                    const MethodInfo* getter =
                        api.il2cpp_property_get_get_method != nullptr
                            ? GuardedPropertyAccessor(api, property, true)
                            : nullptr;
                    const MethodInfo* setter =
                        api.il2cpp_property_get_set_method != nullptr
                            ? GuardedPropertyAccessor(api, property, false)
                            : nullptr;

                    body += "    public ";
                    // The property's type is the getter's return type; a setter-only
                    // property has to take it from the setter's last parameter.
                    const Il2CppType* type = nullptr;
                    if (getter != nullptr && api.il2cpp_method_get_return_type != nullptr) {
                        type = api.il2cpp_method_get_return_type(getter);
                    } else if (setter != nullptr && api.il2cpp_method_get_param != nullptr &&
                               api.il2cpp_method_get_param_count != nullptr) {
                        const std::uint32_t count = api.il2cpp_method_get_param_count(setter);
                        if (count > 0) {
                            type = api.il2cpp_method_get_param(setter, count - 1);
                        }
                    }
                    if (type != nullptr && api.il2cpp_type_get_name != nullptr) {
                        const char* type_name = api.il2cpp_type_get_name(type);
                        body += type_name != nullptr ? type_name : "?";
                    } else {
                        body += "?";
                    }
                    body += " ";
                    body += property_name != nullptr ? property_name : "?";
                    body += " { ";
                    if (getter != nullptr) {
                        body += "get; ";
                    }
                    if (setter != nullptr) {
                        body += "set; ";
                    }
                    body += "}\n";
                    ++stats.properties;
                    ++property_count;
                }
            }

            body += "}\n\n";
            // One class done.  The interval mask inside `report` turns this into a
            // published update every `progress_class_interval` classes, which is what
            // makes a long walk visible instead of indistinguishable from a hang.
            report(false);
        }
        // Forced at the image boundary so a large image cannot leave the last partial
        // interval unreported, and so the image counter advances exactly once per image
        // rather than only when a class happened to land on the mask.
        report(true);
    }

    // Publish the fault count last, so it covers the whole walk.
    stats.contained_faults = g_contained_faults.load();

    // Assemble the file: identity, provenance, then the walk.
    //
    // INCOMPLETE goes in the FIRST line rather than at the end.  The previous revision
    // appended the warning to the tail of a 9-30 MB file, i.e. exactly where a reader
    // who opens the head never looks -- and the whole reason for counting contained
    // faults is that a silently partial dump is the one outcome worse than a visible
    // failure.  The tail line is kept as well, because a reader that greps for it (the
    // existing scripts do) must keep finding it.
    out += "// dump.cs generated from the LIVE IL2CPP type system\n";
    out += "// field offsets are runtime-verified; they are the reason this exists\n";
    out += "// alongside Il2CppDumper rather than replacing it\n";
    if (stats.contained_faults != 0) {
        out += "// INCOMPLETE: " + std::to_string(stats.contained_faults) +
               " runtime call(s) faulted and were skipped; classes may be missing\n";
    }
    AppendProvenanceHeader(out, stats, options);
    out += "\n";
    out += body;
    if (stats.contained_faults != 0) {
        out += "// WARNING: " + std::to_string(stats.contained_faults) +
               " runtime call(s) faulted and were skipped; this dump is INCOMPLETE\n";
    }
    return stats;
}

}  // namespace cabbird::il2cpp
