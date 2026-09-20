/* Unity build addresses, as data.
 *
 * WHY THIS EXISTS
 * ---------------
 * IL2CPP publishing strips names: the C# method `Runtime.Extension.UPlayerLoop.Dispatch` is
 * just a native function at some RVA, and that RVA moves on every game update.  The first
 * version of `DispatchTickHook` hardcoded it in the source, which is exactly the kind of
 * thing that works until it doesn't and then leaves no way to tell why.
 *
 * There are three ways to find such a function, and this project needs to be explicit about
 * which one it uses:
 *
 *   1. A SIGNATURE SCAN.  The sibling Unreal project had to do this because UE strips
 *      everything.  It is a bad fit here: IL2CPP emits many small functions that share
 *      prologues, so a scan returns candidates, not an answer, and "pick the first match"
 *      is a coin flip that fails silently.
 *   2. A HARDCODED ADDRESS.  Honest but unmaintainable, and it fails silently on update.
 *   3. RESOLVE THE METADATA.  `il2cpp_domain_get` / `il2cpp_class_from_name` /
 *      `il2cpp_class_get_method_from_name` -> `MethodInfo*` -> code pointer.  This is the
 *      correct long-term answer and it is build-independent.
 *
 * This file implements (3) as the primary path and falls back to (2) when the metadata APIs
 * are unavailable, with the fallback VERIFIED before use so it can only ever succeed or
 * refuse -- never hook the wrong thing.  Option (3) is attempted first precisely so the
 * hardcoded table becomes a safety net that is expected to go unused, rather than the
 * mechanism the framework depends on.
 *
 * The table lives in `profiles/unity-build-profiles.json` next to the runtime, keyed by
 * build id, so adding a game build is a data edit.
 */

#ifndef CABBIRD_GAME_UNITY_UNITY_BUILD_PROFILE_HPP
#define CABBIRD_GAME_UNITY_UNITY_BUILD_PROFILE_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird {

// One addressable function in a build: where it is, and how to tell whether the thing at
// that address is still the function it used to be.
struct UnityResolvedMethod {
    bool resolved{};           // True when `address` is safe to use.
    std::uintptr_t address{};  // Absolute, ready to hand to a hook.
    std::uint64_t rva{};       // What the profile said, for diagnostics.
    std::string how;           // "metadata", "profile", or the reason it failed.
    std::string detail;        // Type/method/signature, for a log line.
};

/* Loads per-build method addresses and resolves them against the running process.
 *
 * Resolution order per lookup:
 *   1. IL2CPP metadata (`il2cpp_class_from_name` + `il2cpp_class_get_method_from_name`),
 *      which is build-independent.  The method pointer is taken from the live `MethodInfo`,
 *      so no address is assumed.
 *   2. The profile entry, with its `prologue` bytes re-read from the live process and
 *      compared before the address is returned.
 *
 * A miss returns `resolved == false` with the reason in `how`.  It never returns an address
 * it has not verified by one of the two routes above.
 */
class UnityBuildProfile final {
public:
    // A borrowed view of one profile entry, so the resolvers can take an entry without
    // depending on how the loader stores it.
    struct EntryView {
        std::string type_name;
        std::string method_name;
        std::string signature;
        std::uint64_t rva{};
        std::vector<unsigned char> prologue;
    };

    // Parses the profile document.  Does not touch the game; resolution is per call.
    [[nodiscard]] static UnityBuildProfile LoadFromFile(
        const std::filesystem::path& path, std::string* error);

    // The profile to use: the newest entry, chosen by `id`, or the only one present.  A
    // separate entry point so the selection rule is testable without a running game.
    [[nodiscard]] static UnityBuildProfile Select(
        const std::filesystem::path& path, std::string_view preferred_id, std::string* error);

    [[nodiscard]] bool Valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& BuildId() const noexcept { return build_id_; }
    [[nodiscard]] const std::string& GameAssemblyName() const noexcept {
        return assembly_name_;
    }
    [[nodiscard]] const std::string& Error() const noexcept { return error_; }

    // Resolve one named method.  `key` is the profile's method key, e.g.
    // "unity.playerloop.dispatch".
    [[nodiscard]] UnityResolvedMethod Resolve(std::string_view key) const;

    // Names of every method this profile knows, for diagnostics.
    [[nodiscard]] std::vector<std::string> MethodKeys() const;

    // The first bytes the profile recorded for a method, or an empty vector when it has none.
    //
    // Exposed because a caller that is about to CALL a resolved address needs the same evidence
    // the resolver uses on its profile route.  A pointer that merely landed inside the module is
    // "plausible"; the bytes at it are what make it "this build's body".  The entity walk calls
    // `UnityEngine.Transform::get_position` directly, and it can only know that the calling
    // convention it was written against is the one this build uses by checking that the body it
    // is about to call is byte-for-byte the body the convention was read from.
    [[nodiscard]] std::vector<unsigned char> PrologueFor(std::string_view key) const;

private:
    struct Entry {
        std::string key;
        std::string type_name;
        std::string method_name;
        std::string signature;
        std::uint64_t rva{};
        std::vector<unsigned char> prologue;
    };

    bool valid_{};
    std::string build_id_;
    std::string assembly_name_{"GameAssembly.dll"};
    std::string error_;
    std::vector<Entry> entries_;
};

// The path a host should look in: `<runtime_root>/profiles/unity-build-profiles.json`.
[[nodiscard]] std::filesystem::path DefaultUnityBuildProfilePath(
    const std::filesystem::path& runtime_root);

}  // namespace cabbird

#endif  // CABBIRD_GAME_UNITY_UNITY_BUILD_PROFILE_HPP
