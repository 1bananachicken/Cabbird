#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

// ---------------------------------------------------------------------------
// A rising-edge latch for the platform menu hotkey (default Insert).
//
// This file is Cabbird-owned: it has no counterpart in Anomaly, so nothing here drifts from the
// port.  include/cabbird/platform_ui_input_policy.hpp -- which IS a copied file and stays
// byte-identical to upstream -- keeps its upstream signature, and this header feeds it a state
// that actually works.
//
// WHY IT IS NEEDED.  Upstream's ShouldTogglePlatformMenus tests the LOW-ORDER bit of
// GetAsyncKeyState's result.  That bit means "the key was pressed since the last call to
// GetAsyncKeyState", it is one latch per key shared process-wide, and any caller clears it.
// Upstream documents this exact hazard against itself in src/ui/platform_host.cpp:4491-4494:
//
//     "The async-input reconciler polls GetAsyncKeyState every frame, which clears the
//      "pressed since last call" low-order bit, so that bit cannot be used to detect key
//      presses here."
//
// ...and then uses 0x8000 plus its own rising edge for the hotkey CAPTURE path -- while the menu
// TOGGLE path kept reading the low bit.  There are two callers of that toggle, the present hook
// (src/render/dx11/embedded_renderer.cpp) and the host message loop (src/ui/platform_host.cpp),
// and the per-frame poller clears the bit before either of them looks.  The result is the
// reported symptom: "Insert only opens or closes the menu every few presses".
//
// The latch below is ours, so the press edge is derived from the key's real-time down state
// (0x8000) against our own previous sample, and one press is handed to exactly one consumer.
// ---------------------------------------------------------------------------

namespace cabbird {

namespace detail {

struct MenuHotkeyLatchState final {
    bool previous_down{false};
    std::atomic<bool> press_pending{false};
    std::atomic<std::uint64_t> press_tick{0};
};

// A function-local static inside an inline function has a single instance program-wide, so both
// call sites -- different translation units, different libraries -- share it without either
// having to own it.
inline MenuHotkeyLatchState& MenuHotkeyLatch() noexcept {
    static MenuHotkeyLatchState state;
    return state;
}

}  // namespace detail

// Samples the key's real-time down state.  Safe to call from more than one place: only the
// transition from up to down sets the latch, so extra sampling cannot turn one press into
// several toggles.
inline void PollMenuHotkey(unsigned key) noexcept {
    detail::MenuHotkeyLatchState& state = detail::MenuHotkeyLatch();
    const bool down = (::GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    if (down && !state.previous_down) {
        state.press_tick.store(::GetTickCount64(), std::memory_order_relaxed);
        state.press_pending.store(true, std::memory_order_relaxed);
    }
    state.previous_down = down;
}

// Returns the state to hand to ShouldTogglePlatformMenus: nonzero exactly when a fresh press is
// being claimed, by a caller whose window owns focus.
//
// A press is held for a short grace period instead of being dropped the instant an unfocused site
// polls: the two sites poll in the same frame and do not always agree on focus, so dropping on
// the first unfocused poll would re-introduce the lost presses this header exists to fix.  It is
// not banked indefinitely either -- a press older than the grace period is discarded, because
// firing a toggle long after the user pressed the key is worse than dropping it.
[[nodiscard]] inline int TakeMenuHotkeyToggleState(const bool focused) noexcept {
    detail::MenuHotkeyLatchState& state = detail::MenuHotkeyLatch();
    if (!state.press_pending.load(std::memory_order_relaxed)) return 0;
    constexpr std::uint64_t kPressGraceMilliseconds = 250;
    const std::uint64_t pressed_at = state.press_tick.load(std::memory_order_relaxed);
    if (::GetTickCount64() - pressed_at > kPressGraceMilliseconds) {
        state.press_pending.store(false, std::memory_order_relaxed);
        return 0;
    }
    if (!focused) return 0;
    state.press_pending.store(false, std::memory_order_relaxed);
    return 1;
}

}  // namespace cabbird
