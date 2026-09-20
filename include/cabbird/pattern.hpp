// Byte-pattern scanner.
//
// Ported from Anomaly's `include/pattern.hpp` (namespace unitymem) with the logic
// unchanged.  On the Unity/IL2CPP target this is the FALLBACK path, not the
// primary one: `GameAssembly.dll` exports a full, unstripped `il2cpp_*` API
// (241 exports on the target build), so symbol resolution normally goes through
// the export table.  Scanning is still needed for anything the runtime does not
// export -- e.g. `MethodInfo::methodPointer`, the metadata registration struct,
// or if a future build ships a stripped export table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cabbird::mem {

struct PatternByte {
    std::uint8_t value{};
    std::uint8_t mask{};
};

class Pattern {
public:
    // Parses a textual pattern such as "48 8B 05 ?? ?? ?? ?? 48 85 C0".
    // Throws std::invalid_argument on malformed input.
    static Pattern Parse(std::string_view text);

    [[nodiscard]] std::vector<std::size_t> FindAll(
        std::span<const std::uint8_t> bytes,
        std::size_t limit = 256) const;
    [[nodiscard]] std::size_t Size() const noexcept { return bytes_.size(); }

private:
    std::vector<PatternByte> bytes_;
    // The longest contiguous fully-specified run is searched first; every
    // candidate is still checked against the complete masked pattern.
    std::size_t anchor_offset_{};
    std::vector<std::uint8_t> anchor_;
};

}  // namespace cabbird::mem

/* `Pattern` also needs to be reachable as `cabbird::Pattern` and, through the alias, as
 * `unitymem::Pattern`: Anomaly declares it in the same namespace as the service wrappers, so
 * copied code spells it both ways.  DECIDED (user): this layout is Cabbird's to choose.
 * See the rationale in cabbird/memory.hpp. */
namespace cabbird {

using Pattern = cabbird::mem::Pattern;
using PatternByte = cabbird::mem::PatternByte;

}  // namespace cabbird
