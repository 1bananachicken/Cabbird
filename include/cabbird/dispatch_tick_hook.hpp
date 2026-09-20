/* The per-frame tick source, taken from the game's OWN frame dispatcher.
 *
 * WHY THIS EXISTS
 * ---------------
 * `Cabbird` drives plugins from two domains.  The render domain hangs off the DXGI
 * `Present` hook and works: `pump.drawCalls` counts every frame.  The game domain is
 * supposed to hang off the adapter's tick callback, and without this header it does not:
 * `pump.gameUpdateCalls` stays at 0 while `drawCalls` keeps climbing, so no plugin's
 * `on_update` ever runs.
 *
 * The cause was a missing link, not a broken one.  `UnityAdapter::OnGameTick` was declared
 * in the header and had no definition, and the adapter itself is not constructible -- so the
 * `adapter->SetTickCallback(...)` block in `embedded_bridge.cpp`, gated on
 * `adapter != nullptr`, could never run.  The tick machinery was complete and had no caller.
 *
 * WHERE THE TICK COMES FROM
 * -------------------------
 * The game ships its own frame dispatcher in its IL2CPP metadata:
 *
 *     public class Runtime.Extension.UPlayerLoop : System.Object
 *     {
 *     private static readonly List<Action<Single>> s_Ticks;
 *     public  static void RegisterTick(System.Action<System.Single>);
 *     public  static void UnregisterTick(System.Action<System.Single>);
 *     private static void EnsureUpdateInjected();
 *     private static void Dispatch();     <-- walks s_Ticks, calls every registered callback
 *     }
 *
 * `Dispatch` is the game's own frame boundary, which is a better tick than any per-entity
 * `Update` or any render-thread approximation.
 *
 * HOW THE ADDRESS IS FOUND
 * ------------------------
 * Through `UnityBuildProfile`, which tries the IL2CPP metadata first (build-independent) and
 * falls back to a per-build entry whose prologue bytes are re-verified against the live
 * process before use.  This class no longer contains an address -- see that header for why
 * hardcoding one is a hazard rather than a shortcut.
 *
 * WHY NOT REUSE `GameTickHook`'s THUNK
 * ------------------------------------
 * `GameTickHook` is ported from the Unreal sibling and its thunk is
 * `void __fastcall(void* self, float delta, bool idle)` -- UWorld::Tick's signature.  It is
 * NOT usable here and must not be pointed at `Dispatch`, which takes no arguments: a detour
 * that reads `delta` from an XMM register the caller never set would hand plugins a garbage
 * delta while looking like it works.  This class therefore installs its own thunk that
 * preserves the ABI 1:1 (no argument register is read, written or moved) and measures the
 * delta from the host's own frame clock.
 */

#ifndef CABBIRD_GAME_UNITY_DISPATCH_TICK_HOOK_HPP
#define CABBIRD_GAME_UNITY_DISPATCH_TICK_HOOK_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace cabbird {

class HookManager;

// Drives a callback from the game's own per-frame dispatcher, resolved through a build
// profile.  Construct, `Start()`, and the callback runs once per game frame.
//
// WHY THIS IS NOT `final`, AND WHY `Start`/`Stop` ARE VIRTUAL.  The delivery path this class feeds
// -- adapter -> endpoint -> `PluginManager::GameUpdate` -- was broken for the entire time it took
// to notice, because the only way to exercise it was to launch the game: `Start()` resolves an
// address inside the game's module, which a test process does not have.  A test that can pass
// `Start()` without a game is the difference between asserting that path and hoping.  A derived
// stand-in overrides only this class's reported lifecycle; the adapter's own endpoint, thread
// gate and epoch logic -- the parts that were wrong -- stay the real ones.
//
// The cost is nil on the hot path: `Start`/`Stop` are called once per generation, never per frame,
// and the per-frame work is inside the ABI-neutral thunk, which is not virtual.
class DispatchTickHook {
public:
    using Callback = std::function<void(double)>;

    // `profile_path` is the per-build address document; see UnityBuildProfile.  The lookup key
    // is fixed to the game's dispatcher -- this class exists for one function and pretending
    // otherwise would invite hooking arbitrary methods through a tick driver.
    DispatchTickHook(std::filesystem::path profile_path, Callback callback);
    virtual ~DispatchTickHook();

    DispatchTickHook(const DispatchTickHook&) = delete;
    DispatchTickHook& operator=(const DispatchTickHook&) = delete;

    // Loads the profile, resolves the dispatcher, and installs the hook.  Returns false --
    // with a reason in `LastError()` -- when the profile is missing, the method cannot be
    // resolved or verified, or MinHook refuses.  Never throws.
    [[nodiscard]] virtual bool Start() noexcept;
    virtual void Stop() noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] virtual const std::string& LastError() const noexcept;

    // Frames observed since `Start()`.  The host's `pump.gameUpdateCalls` already counts these;
    // this is here so a failing install can be told apart from a callback that fires and does
    // nothing.
    [[nodiscard]] std::uint64_t TickCount() const noexcept;

    // Where the dispatcher was resolved from ("metadata" / "profile"), the address, and the
    // build id the profile named.  Diagnostics, and empty before `Start()`.
    [[nodiscard]] const std::string& ResolutionHow() const noexcept;
    [[nodiscard]] std::uintptr_t ResolutionAddress() const noexcept;
    [[nodiscard]] const std::string& BuildId() const noexcept;

    // The profile method key this class resolves.  Public so a caller can log it without
    // repeating the string.
    [[nodiscard]] static const char* MethodKey() noexcept;

protected:
    // The callback this clock was constructed with.  Exposed to derived stand-ins so a test can
    // drive exactly the callback the adapter built, rather than inventing one of its own -- the
    // object under test is the adapter's wiring, and a test-supplied callback would not test it.
    [[nodiscard]] const Callback& InstalledCallback() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird

#endif  // CABBIRD_GAME_UNITY_DISPATCH_TICK_HOOK_HPP
