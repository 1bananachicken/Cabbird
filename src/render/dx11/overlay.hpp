// D3D11/DXGI Present hook + ImGui overlay host.
//
// Design notes:
//  * The swapchain vtable slot is obtained from a throw-away D3D11 swapchain
//    ("kiero" technique) because `IDXGISwapChain::Present` is not an exported
//    symbol.  Hooking that shared slot covers every swapchain the game creates
//    and survives swapchain recreation on resolution changes.
//  * D3D12 uses the same `IDXGISwapChain::Present` slot, so the hook still sees
//    D3D12 frames; we detect that case and log it instead of pretending the
//    overlay works.
//  * Everything is behind `Options` so the staged probe can run with zero hooks installed.
#pragma once

#include <string>

#include "options.hpp"

namespace cabbird {

// Spawns the worker that hooks Present and drives the overlay.  Returns
// immediately; failures are reported through the log.
bool StartOverlayHost(const Options& options);

// Best-effort teardown (called from DllMain on process detach).
void StopOverlayHost();

// Optional per-presented-frame callback, invoked on the thread that presents.
//
// It exists so the manual-map entry point can start the IL2CPP probe only once
// the game is demonstrably rendering, without this shared overlay code having to
// link the probe.  The callback must be cheap and must not block: it runs inside
// the game's Present path.
using FrameCallback = void (*)(unsigned long long frame_index);
void SetFrameCallback(FrameCallback callback);

}  // namespace cabbird
