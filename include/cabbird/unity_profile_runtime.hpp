#pragma once

#include "cabbird/unity_adapter.hpp"
#include "cabbird/hook_manager.hpp"
#include "cabbird/unity_build_profile.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cabbird {

struct UnityProfileRuntimeOptions {
    std::filesystem::path runtime_root;
    std::string game_id{"ap"};
    HMODULE game_module{};
    // NOT WIRED, AND SAYING SO IS THE POINT.  `[Performance] *SnapshotTickInterval` reaches this
    // field and stops: nothing reads it, so the ini keys and the docs table describe a cadence
    // this host does not implement.  Upstream hands the same struct to its adapter, which
    // throttles snapshots with it, and so does this port now: `entity_tick_interval` becomes the
    // entity walk's sampling divisor, and the value travels
    // `[Performance] EntitySnapshotTickInterval` -> `AnalyzerConfig` -> here -> the adapter's
    // constructor -> the gate in `RefreshUnityEntityEsp`.
    //
    // WHAT THIS FIELD USED TO BE.  It was carried here and dropped on the floor: the adapter
    // constructor that took it was declared and never defined, and the field the gate read
    // (`EntityComponentState::refresh_divisor`) was never assigned -- so the walk ran every tick
    // while its own comment claimed "DEFAULT 8 ... THE WHOLE FRAME-RATE FIX".  Both halves of that
    // are fixed: the value is passed at construction and the field is gone.
    //
    // Only the entity interval has a consumer.  `player_tick_interval` describes a cadence this
    // port does not need (the player snapshot is a handful of field reads on the walk's own tick),
    // and `actor_tick_interval` describes an upstream split this port's single entity walk already
    // covers; both are kept as declared data so the shape still matches upstream.
    UnitySnapshotSamplingOptions snapshot_sampling;
    // Profile lifecycle owns the adapter; render code only borrows it.
    bool start_frame_clock{true};
    std::chrono::milliseconds frame_clock_budget{std::chrono::seconds(20)};
    UnityAdapter::FrameClockFactory frame_clock_factory;
};

// One method a Unity profile declares, and what the host actually did with it.
struct UnityProfileMethodEvidence {
    std::string key;
    bool bound{};
    // Whether the host TRIES to bind this method at startup.
    //
    // The profile document is also the place per-build addresses are RECORDED, and a record that
    // the host resolves lazily -- `unity.transform.get_position` is resolved by the entity walk,
    // not by the startup binder -- is not a failed binding.  Counting it as one made the profile
    // report `degraded` the moment such a record was added, which reads as a broken profile in
    // the compatibility page while nothing was broken.  The state below is computed over the
    // methods the host ATTEMPTS, and a record it does not attempt is reported as documented
    // rather than as unbound.
    bool attempted{true};
    std::string how;  // "metadata" / "profile", or why no binding was attempted/failed
    std::uintptr_t address{};
};

/* The evidence the Unity compatibility page is built from.
 *
 * THERE IS ONE KIND OF PROFILE HERE, AND THIS STRUCT IS ALL OF IT.
 *
 * `unity_*` is CABBIRD's: the `profiles/unity-build-profiles.json` document the frame clock
 * binds against, plus the binding result for every method it declares.  Unity methods are
 * resolved through IL2CPP metadata (or a prologue-verified RVA), not by pattern scanning --
 * see unity_build_profile.hpp -- so this is the layer that can report a real build id, a real
 * profile hash and a real per-method result.
 *
 * An upstream `BuildProfile`/`SymbolResolver` layer used to sit beside it: a document whose
 * `symbols` were byte patterns, scanned against the running module.  This tree shipped no such
 * document, the scan therefore always produced an empty resolution, and the only consumer was
 * the diagnostics JSON.  It was deleted rather than left as a second, always-empty answer to
 * "which build am I on?"; see docs/api-reference/unity-services.md.
 */
struct UnityProfileEvidenceSnapshot {
    std::filesystem::path unity_profile_path;
    // SHA-256 of the document as it was read, so the page can name the exact data the
    // binding came from.  Empty when the document could not be read.
    std::string unity_profile_hash;
    // Why `unity_profile` is empty: missing file, malformed JSON, entry without an id.
    std::string unity_profile_error;
    std::optional<UnityBuildProfile> unity_profile;
    std::vector<UnityProfileMethodEvidence> unity_methods;
};

class UnityProfileRuntime final {
public:
    explicit UnityProfileRuntime(UnityProfileRuntimeOptions options);
    ~UnityProfileRuntime();

    UnityProfileRuntime(const UnityProfileRuntime&) = delete;
    UnityProfileRuntime& operator=(const UnityProfileRuntime&) = delete;

    // An unknown or partial build is a successful degraded start.
    [[nodiscard]] bool Start(std::stop_token stop_token = {}) noexcept;
    // A timed-out adapter remains owned and prevents restart until a later Stop drains it.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::shared_ptr<UnityAdapter> Adapter() const;
    [[nodiscard]] UnityProfileEvidenceSnapshot Evidence() const;
    [[nodiscard]] std::string DiagnosticsJson() const;
    [[nodiscard]] std::vector<HookRecordView> Hooks() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
