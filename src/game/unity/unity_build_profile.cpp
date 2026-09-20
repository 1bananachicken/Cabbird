/* UnityBuildProfile -- see the header for why addresses are data and not constants. */

#include "cabbird/unity_build_profile.hpp"

#include "cabbird/il2cpp.hpp"
#include "cabbird/memory.hpp"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace cabbird {
namespace {

using Json = nlohmann::json;

// Parses "48895C24..." (or with spaces/dashes) into bytes.  Returns false rather than
// truncating on a malformed string: a half-read prologue would compare unequal and be
// reported as a version mismatch, which is a wrong diagnosis for a typo in a config file.
bool ParseHexBytes(std::string_view text, std::vector<unsigned char>* out) {
    out->clear();
    std::string digits;
    digits.reserve(text.size());
    for (const char character : text) {
        if (character == ' ' || character == '-' || character == '\t') continue;
        digits.push_back(character);
    }
    if (digits.empty() || digits.size() % 2 != 0) return false;
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = nibble(digits[index]);
        const int low = nibble(digits[index + 1]);
        if (high < 0 || low < 0) return false;
        out->push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool ParseRva(const Json& value, std::uint64_t* out) {
    if (value.is_number_unsigned()) {
        *out = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed, 0);  // base 0 honours "0x"
        if (consumed != text.size()) return false;
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

// Module base and size, or false when the module is not loaded.  The size check is what makes
// a method pointer trustworthy: a code pointer lands inside the module's range and heap
// garbage almost never does.
bool ModuleBounds(const std::wstring& name, std::uintptr_t* base, std::uintptr_t* size) {
    const HMODULE module = GetModuleHandleW(name.c_str());
    if (module == nullptr) return false;
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (base != nullptr) *base = reinterpret_cast<std::uintptr_t>(module);
    if (size != nullptr) *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

// `MethodInfo::methodPointer`, which the runtime exposes no getter for.
//
// The offset is 0 because Unity declares it as the first member.  That declaration comes from
// a Unity version other than the one this target runs, so the value is only accepted when it
// lands inside the module -- the same reasoning, and the same measurement, as
// `src/mem/il2cpp_dump.cpp`'s `ReadMethodPointer`.  Duplicated deliberately rather than
// shared: that one lives in an anonymous namespace inside the dump translation unit, and
// lifting it out would tie the dump's build to this file's.
bool ReadMethodPointer(const void* method, std::uintptr_t module_base,
                       std::uintptr_t module_size, std::uintptr_t* out) {
    *out = 0;
    if (method == nullptr) return false;
    std::uintptr_t pointer = 0;
    const auto bytes = mem::ReadMemory(reinterpret_cast<std::uintptr_t>(method), sizeof(pointer));
    if (!bytes.has_value() || bytes->size() != sizeof(pointer)) return false;
    std::memcpy(&pointer, bytes->data(), sizeof(pointer));
    if (pointer == 0) return false;
    if (module_base == 0 || module_size == 0) {
        *out = pointer;
        return true;
    }
    if (pointer < module_base || pointer >= module_base + module_size) return false;
    *out = pointer;
    return true;
}

std::string Hex(std::uint64_t value) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (value == 0) return "0";
    std::string text;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFULL);
        if (nibble != 0 || started) {
            started = true;
            text.push_back(kDigits[nibble]);
        }
    }
    return text;
}

std::string HexBytes(const unsigned char* bytes, std::size_t count) {
    std::string text;
    text.reserve(count * 3);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) text.push_back(' ');
        text += Hex(bytes[index]);
    }
    return text;
}

// Metadata route: ask the runtime for the class and the method, then read the code pointer
// out of the returned `MethodInfo`.
//
// This is the route that survives a game update, which is the whole point.  It is tried first
// and is expected to succeed on any build whose metadata still contains the method.
UnityResolvedMethod ResolveThroughMetadata(const UnityBuildProfile::EntryView& entry,
                                           const std::wstring& assembly_name) {
    UnityResolvedMethod result;
    if (!il2cpp::Ready()) {
        result.how = "metadata unavailable (il2cpp runtime not attached)";
        return result;
    }

    std::uintptr_t module_base = 0;
    std::uintptr_t module_size = 0;
    if (!ModuleBounds(assembly_name, &module_base, &module_size)) {
        result.how = "metadata unavailable (module not loaded)";
        return result;
    }

    const auto& api = il2cpp::Functions();
    // Every entry this route calls, checked before any of them is called.  The runtime being
    // attached says the module is there, not that it exports this particular set -- and calling a
    // null entry point is a crash, not a refusal.  `il2cpp_domain_get_assemblies` is checked like
    // the rest for the same reason: it was the one that took the game down.
    if (api.il2cpp_domain_get == nullptr || api.il2cpp_domain_get_assemblies == nullptr ||
        api.il2cpp_assembly_get_image == nullptr || api.il2cpp_class_from_name == nullptr ||
        api.il2cpp_class_get_method_from_name == nullptr) {
        result.how = "metadata unavailable (the runtime does not export the lookup entry points)";
        return result;
    }
    const std::string full = entry.type_name;
    const auto separator = full.rfind('.');
    const std::string name_space = separator == std::string::npos ? std::string{} : full.substr(0, separator);
    const std::string type_name = separator == std::string::npos ? full : full.substr(separator + 1);

    // THE COUNT POINTER IS NOT OPTIONAL.
    //
    // `il2cpp_domain_get_assemblies(domain, size)` WRITES the assembly count through `size` as its
    // first act.  This call used to pass `nullptr` and then walk the returned array until it saw a
    // null entry -- so it both wrote the count to address zero and assumed a terminator the array
    // does not have.  The route was never reached with a live runtime until the entity walk began
    // resolving `unity.transform.get_position` through it, and then it took the game down with an
    // access violation (write to 0x0) inside `il2cpp_domain_get_assemblies`, three seconds after
    // the entity overlay was enabled: `GameAssembly+0x132AED9`, `RDX = 0`.
    //
    // The array is NOT null-terminated; `size` is the only end it has.
    std::size_t assembly_count = 0;
    const auto* assemblies =
        api.il2cpp_domain_get_assemblies(api.il2cpp_domain_get(), &assembly_count);
    if (assemblies == nullptr) {
        result.how = "metadata unavailable (no assemblies)";
        return result;
    }
    // The method's own image, found by scanning assemblies for the class.  Asking the image
    // first would require knowing the image name; scanning is what the dump does and it does
    // not assume a layout.
    for (std::size_t index = 0; index < assembly_count; ++index) {
        const auto* image = api.il2cpp_assembly_get_image(assemblies[index]);
        if (image == nullptr) continue;
        auto* const klass = api.il2cpp_class_from_name(image, name_space.c_str(), type_name.c_str());
        if (klass == nullptr) continue;
        const auto* method = api.il2cpp_class_get_method_from_name(klass, entry.method_name.c_str(), 0);
        if (method == nullptr) continue;
        std::uintptr_t pointer = 0;
        if (!ReadMethodPointer(method, module_base, module_size, &pointer)) {
            result.how = "metadata found the method but its code pointer is outside the module";
            return result;
        }
        result.resolved = true;
        result.address = pointer;
        result.rva = pointer - module_base;
        result.how = "metadata";
        result.detail = entry.type_name + "." + entry.method_name +
                        (entry.signature.empty() ? "" : "  " + entry.signature);
        return result;
    }
    result.how = "metadata has no method \"" + entry.type_name + "." + entry.method_name + "\"";
    return result;
}

// Profile route: use the recorded RVA, but only after re-reading the recorded prologue bytes
// from the live process.  A relocated or reshaped function therefore fails safe -- refused,
// with a diff in the reason -- instead of being hooked.
UnityResolvedMethod ResolveThroughProfile(const UnityBuildProfile::EntryView& entry,
                                          const std::wstring& assembly_name) {
    UnityResolvedMethod result;
    result.rva = entry.rva;
    result.detail = entry.type_name + "." + entry.method_name;
    if (entry.rva == 0) {
        result.how = "profile has no rva for this method";
        return result;
    }

    std::uintptr_t module_base = 0;
    std::uintptr_t module_size = 0;
    if (!ModuleBounds(assembly_name, &module_base, &module_size)) {
        result.how = "module not loaded";
        return result;
    }
    if (entry.rva >= module_size) {
        result.how = "profile rva 0x" + Hex(entry.rva) + " is outside the module (size 0x" +
                     Hex(module_size) + "); profile does not match this build";
        return result;
    }

    const auto address = module_base + entry.rva;
    if (!entry.prologue.empty()) {
        const auto bytes = mem::ReadMemory(address, entry.prologue.size());
        if (!bytes.has_value() || bytes->size() != entry.prologue.size()) {
            result.how = "cannot read 0x" + Hex(address) + " to verify the prologue";
            return result;
        }
        if (std::memcmp(bytes->data(), entry.prologue.data(), entry.prologue.size()) != 0) {
            result.how = "prologue mismatch at 0x" + Hex(address) + ": expected [" +
                         HexBytes(entry.prologue.data(), entry.prologue.size()) + "] found [" +
                         HexBytes(bytes->data(), bytes->size()) + "]";
            return result;
        }
    }
    result.resolved = true;
    result.address = address;
    result.how = "profile";
    return result;
}

}  // namespace

std::filesystem::path DefaultUnityBuildProfilePath(const std::filesystem::path& runtime_root) {
    return runtime_root / L"profiles" / L"unity-build-profiles.json";
}

UnityBuildProfile UnityBuildProfile::LoadFromFile(const std::filesystem::path& path,
                                                  std::string* error) {
    UnityBuildProfile profile;
    const auto fail = [&](std::string message) {
        profile.valid_ = false;
        profile.error_ = std::move(message);
        if (error != nullptr) *error = profile.error_;
        return profile;
    };

    std::ifstream stream(path);
    if (!stream) return fail("cannot open " + path.string());

    Json document;
    try {
        stream >> document;
    } catch (const std::exception& exception) {
        return fail(std::string("cannot parse ") + path.string() + ": " + exception.what());
    }

    const auto profiles = document.find("profiles");
    if (profiles == document.end() || !profiles->is_array() || profiles->empty()) {
        return fail("no \"profiles\" array in " + path.string());
    }

    // One entry for now: the file carries exactly one build.  Kept as a loop so adding a
    // second build does not require changing this function.
    const auto& entry = profiles->front();
    profile.build_id_ = entry.value("id", std::string{});
    profile.assembly_name_ = entry.value("gameAssembly", std::string{"GameAssembly.dll"});
    if (profile.build_id_.empty()) return fail("profile entry has no \"id\"");

    const auto methods = entry.find("methods");
    if (methods == entry.end() || !methods->is_object()) {
        return fail("profile \"" + profile.build_id_ + "\" has no \"methods\" object");
    }

    for (auto iterator = methods->begin(); iterator != methods->end(); ++iterator) {
        const auto& value = iterator.value();
        Entry parsed;
        parsed.key = iterator.key();
        parsed.type_name = value.value("type", std::string{});
        parsed.method_name = value.value("method", std::string{});
        parsed.signature = value.value("signature", std::string{});
        if (parsed.type_name.empty() || parsed.method_name.empty()) {
            return fail("method \"" + parsed.key + "\" is missing \"type\" or \"method\"");
        }
        const auto rva = value.find("rva");
        if (rva != value.end() && !ParseRva(*rva, &parsed.rva)) {
            return fail("method \"" + parsed.key + "\" has an unreadable \"rva\"");
        }
        const auto prologue = value.find("prologue");
        if (prologue != value.end() && prologue->is_string()) {
            if (!ParseHexBytes(prologue->get<std::string>(), &parsed.prologue)) {
                return fail("method \"" + parsed.key + "\" has a malformed \"prologue\" hex string");
            }
        }
        profile.entries_.push_back(std::move(parsed));
    }
    if (profile.entries_.empty()) return fail("profile \"" + profile.build_id_ + "\" has no methods");

    profile.valid_ = true;
    profile.error_.clear();
    if (error != nullptr) error->clear();
    return profile;
}

UnityBuildProfile UnityBuildProfile::Select(const std::filesystem::path& path,
                                            std::string_view preferred_id,
                                            std::string* error) {
    // Loading is the same either way today (one entry per file); the selection rule is stated
    // separately so that adding a build is a matter of adding an entry and naming it here,
    // and so a test can pin the rule without a running game.
    auto profile = LoadFromFile(path, error);
    if (!profile.valid_) return profile;
    if (!preferred_id.empty() && profile.build_id_ != preferred_id) {
        if (error != nullptr) {
            *error = "profile file is \"" + profile.build_id_ + "\" but \"" +
                     std::string(preferred_id) + "\" was requested";
        }
        profile.valid_ = false;
        profile.error_ = error != nullptr ? *error : std::string{};
    }
    return profile;
}

std::vector<std::string> UnityBuildProfile::MethodKeys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& entry : entries_) keys.push_back(entry.key);
    return keys;
}

std::vector<unsigned char> UnityBuildProfile::PrologueFor(std::string_view key) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const Entry& entry) { return entry.key == key; });
    if (found == entries_.end()) return {};
    return found->prologue;
}

UnityResolvedMethod UnityBuildProfile::Resolve(std::string_view key) const {
    if (!valid_) {
        UnityResolvedMethod result;
        result.how = "profile is not loaded: " + error_;
        return result;
    }
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const Entry& entry) { return entry.key == key; });
    if (found == entries_.end()) {
        UnityResolvedMethod result;
        result.how = "profile \"" + build_id_ + "\" has no method \"" + std::string(key) + "\"";
        return result;
    }

    EntryView view{found->type_name, found->method_name, found->signature, found->rva,
                   found->prologue};
    const std::wstring wide_assembly(assembly_name_.begin(), assembly_name_.end());

    // Metadata first: it is the route that survives a game update.  The profile is the
    // fallback, and its prologue check means it can only succeed or refuse.
    auto by_metadata = ResolveThroughMetadata(view, wide_assembly);
    if (by_metadata.resolved) return by_metadata;

    auto by_profile = ResolveThroughProfile(view, wide_assembly);
    if (by_profile.resolved) return by_profile;

    // Both failed: report BOTH reasons.  A single message would send the next reader down
    // whichever path it happened to name, which is how hours get spent on the wrong one.
    by_profile.how = "metadata: " + by_metadata.how + "; profile: " + by_profile.how;
    return by_profile;
}

}  // namespace cabbird
