#pragma once

#include "cabbird/adapter_service_registry.hpp"
#include <filesystem>
#include "cabbird/sdk/cabbird_sdk.h"
#include "cabbird/unity_navigation_input_policy.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

namespace cabbird {

// The plugin host's manager.  Forward-declared because the frame-clock constructor only needs to
// hold a shared reference to it; including plugin_manager.hpp here would pull the whole plugin
// layer into every translation unit that wants to name the adapter.
class PluginManager;

// The frame clock.  Forward-declared for the same reason: `FrameClockFactory` only needs the
// name, and the factory's return type is a `std::unique_ptr`, which the standard library
// supports for incomplete types at the point of declaration.
class DispatchTickHook;

// How often each snapshot is allowed to re-read the game, in game ticks.  1 = every tick.
//
// THE ENTITY DIVISOR IS THE FRAME-RATE CONTROL.  The entity walk costs ~213 ms of the game
// thread per tick, unthrottled, and it is the single most expensive thing this process does to
// the game -- so it runs once every `entity_tick_interval` ticks.  What that divisor does and
// does not age is stated at the gate in `RefreshUnityEntityEsp`: the camera and the projected
// box corners are refreshed EVERY tick, and only the entity set and the entity world positions
// are sampled at the divisor.  A box therefore never lags the camera; only a moving entity's
// position can be up to `entity_tick_interval - 1` ticks old.
//
// The other two are carried from the sibling project's options and have no consumer here yet:
// the player snapshot is a handful of field reads on the same tick as the walk, and this port's
// entity walk already covers what upstream split into an "actor" cadence.  Kept as declared data
// rather than deleted so the shape matches upstream's; nothing reads them.
struct UnitySnapshotSamplingOptions {
    std::uint32_t player_tick_interval{1};
    std::uint32_t entity_tick_interval{1};
    std::uint32_t actor_tick_interval{60};
    // Whether the entity walk may call `UnityEngine.Transform::get_position`'s compiled body
    // directly instead of going through `il2cpp_runtime_invoke`.
    //
    // This is the one place in the port that CALLS game code rather than reading it, so it is a
    // switch and not a constant: `[Performance] DirectPositionCall=0` returns the walk to the
    // reflection route -- with its 1.29 ms per entity -- without a rebuild, which is what makes
    // "did the fast path cause this?" answerable by the person holding the game.
    bool entity_direct_position{true};
};

class UnityAdapter final {
public:
    using TickCallback = std::function<void(double)>;

    /* The only constructor, and it takes exactly what a frame clock needs: the per-build
     * address profile and the plugin manager the frames drive.
     *
     * WHAT USED TO BE HERE.  A ten-parameter constructor took a `BuildFingerprint`, a
     * `BuildProfile`, a `ProfileResolutionSnapshot`, the module/ABI validator registries and
     * three UE5 invocation seams (`ProcessEventInvoker`, `ObjectLookup`, `feature_layout_validators`).
     * It was declared and never defined, so every caller had to pass `{}` and
     * `embedded_bridge.cpp`'s `if (adapter != nullptr)` guard could never open -- which is how
     * the host came to render 42,520 frames while delivering zero game ticks.  It is gone with
     * the upstream profile layer it consumed: a Unity build binds methods through
     * `profiles/unity-build-profiles.json`, not through byte-pattern symbols plus a
     * ProcessEvent funnel, and a declared-but-undefined seam is not a plan.
     *
     * `ResolutionHow()`/`BuildId()` report what the frame clock actually did; an unknown build
     * is a supported degraded start.
     *
     * `snapshot_sampling` is the one option that reaches the game thread's own cost: its entity
     * interval becomes the walk's divisor.  Defaulted, so the value is a configuration decision
     * rather than something every caller has to restate.
     */
    UnityAdapter(std::filesystem::path frame_clock_profile,
                 std::shared_ptr<PluginManager> plugins,
                 UnitySnapshotSamplingOptions snapshot_sampling = {});
    ~UnityAdapter();

    UnityAdapter(const UnityAdapter&) = delete;
    UnityAdapter& operator=(const UnityAdapter&) = delete;

    /* The frame clock's installation seam.
     *
     * `Start` needs a live process: resolving the game's dispatcher reads the game's own module.
     * That made the delivery path -- adapter -> endpoint -> plugin -- impossible to exercise
     * without launching the game, which is exactly the property that let "the endpoint is created
     * but nothing drives it" survive a green build and 42,520 rendered frames.
     *
     * A test supplies a factory returning a stub clock and can then drive the real endpoint, the
     * real thread/epoch gate and the real `OnGameTick` entirely offline.  The default is the real
     * `DispatchTickHook`; production never passes anything.
     *
     * The factory receives the profile path and the per-frame callback the adapter built, and
     * returns the clock (or nullptr to mean "cannot install").  Errors are reported through
     * `LastError()`, which is what the factory is expected to have set on its own object.
     */
    using FrameClockFactory = std::function<std::unique_ptr<DispatchTickHook>(
        const std::filesystem::path&, std::function<void(double)>)>;

    // Starts the frame clock and installs `update` as the per-frame callback.
    [[nodiscard]] bool Start(TickCallback update = {});

    /* How long `Start` keeps retrying the frame clock while the game is still loading.
     *
     * WHY A RETRY IS NEEDED AT ALL.  The host starts the render domain as soon as the proxy DLL
     * runs, which is BEFORE Unity has loaded `GameAssembly.dll` or attached IL2CPP.  A clock
     * that resolves once and stays failed therefore fails permanently, with
     * `metadata unavailable (il2cpp runtime not attached)` / `profile: module not loaded`,
     * and `pump.gameUpdateCalls` stays at 0 forever.  A clock that can only be installed before
     * the game exists is a clock that never installs.
     *
     * So the install retries until `frame_clock_budget` elapses, and only then reports failure.
     * Pass `0` to attempt exactly once -- what a caller does when it already knows the game is up.
     */
    [[nodiscard]] bool Start(TickCallback update,
                             std::chrono::milliseconds frame_clock_budget);

    // As above, with the clock built by `clock_factory`.  This is the form the tests use.
    [[nodiscard]] bool Start(TickCallback update,
                             std::chrono::milliseconds frame_clock_budget,
                             const FrameClockFactory& clock_factory,
                             std::stop_token stop_token = {});
    // Starts the adapter-owned Unity service subsystem without installing the frame clock.  The
    // runtime uses this before plugin discovery so plugin on_load can query the same service
    // generation that the game tick later refreshes.
    [[nodiscard]] bool StartServices();
    [[nodiscard]] bool ServicesStarted() const noexcept;
    [[nodiscard]] std::uint64_t ServiceGeneration() const noexcept;
    // Exceptions contained by the internal refresh loop, observable without log parsing.
    [[nodiscard]] std::uint64_t ServiceRefreshFailures() const noexcept;
    void InvalidateScene() noexcept;
    // Closes tick and SDK admission, revokes tables, then drains accepted work,
    // including asynchronous Dump workers. Retained tables reject calls after
    // revocation without retaining component state or redirecting to a new owner.
    // A false result keeps the generation stopping until a successful later Stop.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    void SetTickCallback(TickCallback callback);
    // Removes the callback immediately, then waits for already-entered game
    // ticks. A finite timeout returns false while the callback is still in
    // flight; the callback has nevertheless been detached and will not be
    // entered by a later tick. Target destruction is deferred off the caller.
    bool ClearTickCallback(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    void OnGameTick(double delta_seconds) noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] DWORD GameThreadId() const noexcept;
    [[nodiscard]] std::uint64_t TickSequence() const noexcept;
    [[nodiscard]] std::uint64_t RejectedThreadTicks() const noexcept;

    // -- frame-clock diagnostics ------------------------------------------------------------
    //
    // Everything below describes the frame clock this adapter installed.
    [[nodiscard]] double LastDeltaSeconds() const noexcept;
    [[nodiscard]] const std::string& BuildId() const noexcept;
    // "metadata" or "profile": how the game's dispatcher was resolved.  A frame clock that
    // resolved through the profile is a build the metadata no longer matched, which is worth
    // knowing before trusting any other address from the same profile.
    [[nodiscard]] const std::string& ResolutionHow() const noexcept;
    [[nodiscard]] std::uintptr_t DispatchAddress() const noexcept;
    [[nodiscard]] const std::string& FrameClockProfilePath() const noexcept;
    [[nodiscard]] const std::string& LastError() const noexcept;

    // Consulted before the plugins are pumped on a frame, and skipped when it returns false.
    // This is the frame clock's own admission gate, separate from the plugin layer's enablement:
    // it lets a caller drop frames without tearing the clock down.
    using FrameFilter = std::function<bool()>;
    void SetFrameFilter(FrameFilter filter);

private:
    struct State;
    // A game tick may still be unwinding after the public adapter owner is
    // released. Each entry point takes a local shared owner before touching
    // State so teardown cannot invalidate the callback's bookkeeping.
    std::shared_ptr<State> state_;
};

}  // namespace cabbird
