/* cabbird/json.hpp -- the smallest thing that can emit valid JSON.
 *
 * Copied from the sibling Anomaly project (include/json.hpp + src/diagnostics/json.cpp)
 * as part of the UE -> Unity port, with the namespace renamed unitymem::json -> cabbird::json.
 * The upstream name was a leftover from the memory library it was born in; the behaviour
 * is unchanged, deliberately.
 *
 * WHY NOT A REAL JSON LIBRARY: every caller of this header writes a diagnostic artifact
 * (a service-graph snapshot, a probe dump) by string concatenation.  Pulling nlohmann/json
 * in would be a build-time cost and, worse, an allocation-heavy dependency inside a process
 * we do not own.  The only thing that must be *correct* here is
 * escaping -- a service id or a failure message is untrusted text, and a stray quote in it
 * turns a machine-readable artifact into a corrupt one.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cabbird::json {

/* Wraps `value` in double quotes, escaping what RFC 8259 requires.  Input is treated as
 * UTF-8; the bytes are passed through, only the control characters are escaped. */
std::string Quote(std::string_view value);

/* Wide overload: converted to UTF-8 first, then quoted.  Kept because the host's own
 * paths (log directory, ini path) arrive as UTF-16. */
std::string Quote(std::wstring_view value);

/* Formats a pointer as a quoted "0x..." string.  Quoted, not bare, so the emitted JSON
 * type of this field does not depend on whether the value happens to fit in a double. */
std::string Hex(std::uintptr_t value);

}  // namespace cabbird::json
