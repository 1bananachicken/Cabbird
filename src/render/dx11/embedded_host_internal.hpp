#pragma once

#include "cabbird/config.hpp"
#include "cabbird/platform_host.hpp"

#include "cabbird/hook_manager.hpp"
#include "cabbird/input_service.hpp"
#include "cabbird/ui_service_registry.hpp"
#include "cabbird/plugin_manager.hpp"
#include "cabbird/host_ui_service.hpp"
#include "cabbird/unitymem_compat.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiContext;

namespace cabbird::embedded {

enum class EmbeddedPerformanceStage : std::uint8_t {
    PresentInterval,
    PresentLease,
    PresentRender,
    PresentOriginal,
    PresentTail,
    ExecuteLease,
    ExecuteCapture,
    ExecuteOriginal,
    RenderLockWait,
    RenderSetup,
    RenderFenceWait,
    RenderFrameReset,
    RenderUi,
    RenderPrepareLockWait,
    RenderPrepareLocked,
    RenderFrameBegin,
    RenderDrawLockWait,
    RenderInput,
    RenderPlatformUi,
    RenderPluginDraw,
    RenderCapture,
    RenderFinalize,
    RenderCommands,
    RenderSubmit,
    RenderTotal,
    GamePump,
    GamePluginLockWait,
    GamePluginUpdate,
    GameEscMenu,
    GameTotal,
    GameTickInterval,
    WorkerPluginLockWait,
    WorkerPluginMaintenance,
    WorkerRetryServices,
    WorkerPollChanges,
    WorkerMaintenance,
    WorkerPersist,
    PlatformUiSubmissionLockWait,
    PlatformUiOperationLockWait,
    PlatformUiWindowState,
    PlatformUiRefreshCatalog,
    PlatformUiRuntimePlugins,
    PlatformUiBuildSnapshot,
    PlatformUiRepositorySnapshot,
    PlatformUiServiceGraphSnapshot,
    PlatformUiAdapterServicesSnapshot,
    PlatformUiUnityCompatibilitySnapshot,
    PlatformUiModelPublish,
    PlatformUiSettingsRefresh,
    PlatformUiRefreshTotal,
    PlatformUiFrameSetup,
    PlatformUiManagementShell,
    PlatformUiPopups,
    PlatformUiWindowPersist,
    Count,
};

struct EmbeddedPerformanceBucket {
    std::atomic_uint64_t samples{};
    std::atomic_uint64_t total_nanoseconds{};
    std::atomic_uint64_t maximum_nanoseconds{};
    std::atomic_uint64_t window_samples{};
    std::atomic_uint64_t window_total_nanoseconds{};
    std::atomic_uint64_t window_maximum_nanoseconds{};
};

class EmbeddedPerformanceProbe final {
public:
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool Enabled() const noexcept;
    void ObservePresent() noexcept;
    void ObserveExecute() noexcept;
    [[nodiscard]] bool SampleRender() noexcept;
    [[nodiscard]] bool SampleGameTick() noexcept;
    void Record(
        EmbeddedPerformanceStage stage,
        std::chrono::steady_clock::duration elapsed) noexcept;
    void RecordMaintenance(std::chrono::steady_clock::duration elapsed) noexcept;
    void RecordPersistence(std::chrono::steady_clock::duration elapsed) noexcept;
    void Publish(const std::shared_ptr<cabbird::StructuredLogger>& logger) noexcept;

private:
    std::array<
        EmbeddedPerformanceBucket,
        static_cast<std::size_t>(EmbeddedPerformanceStage::Count)> buckets_;
    std::atomic_uint64_t present_calls_{};
    std::atomic_uint64_t execute_calls_{};
    std::atomic_uint64_t render_calls_{};
    std::atomic_uint64_t game_tick_calls_{};
    std::atomic_uint64_t maintenance_calls_{};
    std::atomic_uint64_t persistence_calls_{};
    std::atomic_uint64_t present_gap_over_25ms_{};
    std::atomic_uint64_t present_gap_over_50ms_{};
    std::atomic_int64_t last_present_nanoseconds_{};
    std::atomic_int64_t last_game_tick_nanoseconds_{};
    std::atomic_bool enabled_{};
    std::atomic_bool reset_requested_{};
    std::chrono::steady_clock::time_point last_publish_{};
};

template <typename T>
void Release(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

// WndProc only records host input here. Render consumes the mailbox after
// ImGui::NewFrame, which keeps hotkey dispatch and PluginManager access out of
// the window-procedure thread.
struct EmbeddedInputMailbox {
    std::mutex mutex;
    cabbird::InputFrameState frame;
    float last_published_mouse_x{};
    float last_published_mouse_y{};
    std::int64_t pending_wheel_delta{};
    bool mouse_position_published{};
    bool reset_pending{};
    cabbird::InputResetReason reset_reason{cabbird::InputResetReason::None};
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
enum class RendererLifecycle : std::uint8_t {
    Cold,
    Discovering,
    Ready,
    ResizePending,
    DeviceLost,
    Stopping,
    Stopped,
};

// Which Direct3D version the intercepted swap chain belongs to.
//
// The Present hook lives in DXGI, which every Direct3D version shares, so the
// overlay gets called for a D3D11 title exactly as it does for a D3D12 one and
// has to draw with whichever API the back buffer actually belongs to.
enum class EmbeddedRenderApi : std::uint8_t { None, D3D12, D3D11 };

struct HookTargets {
    // Direct3D11 has no command queue, so there is no ExecuteCommandLists to hook.  The
    // field is kept for the D3D12 shape the ported bridge still carries and is
    // deliberately NOT part of the completeness test: requiring it would make hook
    // installation fail on every Direct3D11 title, which is the only title this build
    // supports.  Treated as an optional, API-specific extra.
    void* execute_command_lists{};
    // Present / Present1 / ResizeBuffers / ResizeBuffers1 live on the DXGI swap chain,
    // which every Direct3D version shares, so these four are the real contract.
    void* present{};
    void* present1{};
    void* resize_buffers{};
    void* resize_buffers1{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return present != nullptr && present1 != nullptr &&
            resize_buffers != nullptr && resize_buffers1 != nullptr;
    }
};

// Reports what the submission target's first bytes currently look like and,
// when they are a jump, which module it lands in. Distinguishes a detour that
// is still ours from one that was overwritten by another hook.
[[nodiscard]] std::string DescribeEmbeddedSubmissionTarget();

struct EmbeddedState {
    std::filesystem::path root;
    std::string imgui_ini_path;
    AnalyzerConfig config;
    PlatformDiagnostics diagnostics;
    EmbeddedPerformanceProbe performance;
    // Borrowed from the RuntimeSession composition root. A quarantined UI
    // owner carries its own shared lifetime token; the renderer state itself
    // must not become a second PluginManager owner.
    PluginManager* plugins{};
    std::weak_ptr<PluginManager> plugin_owner;
    // Populated only when the entire renderer generation is quarantined after
    // a hook/UI deadline. Normal renderer operation remains composition-root
    // borrowed.
    std::shared_ptr<PluginManager> quarantined_plugin_owner;
    std::mutex render_mutex;
    std::shared_ptr<std::mutex> plugin_mutex{std::make_shared<std::mutex>()};
    std::mutex queue_mutex;
    IDXGISwapChain* source_swap_chain{};
    IDXGISwapChain3* swap_chain{};
    // Which Direct3D version the intercepted swap chain belongs to.  This build
    // only ever sets D3D11 -- the D3D12 submission machinery (queue, fence,
    // command allocator, descriptor heaps) was removed with that backend -- but
    // the field is kept because the per-frame paths still branch on it and a
    // swap chain that is neither is a real, reportable state.
    EmbeddedRenderApi render_api{EmbeddedRenderApi::None};
    ID3D11Device* d3d11_device{};
    ID3D11DeviceContext* d3d11_context{};
    ID3D11RenderTargetView* d3d11_render_target{};
    bool dx11_initialized{};
    DXGI_FORMAT render_target_format{DXGI_FORMAT_UNKNOWN};
    HWND window{};
    ImGuiContext* imgui_context{};
    WNDPROC original_window_proc{};
    // Set when the window procedure could not be exchanged. The overlay still
    // renders in that case, so the reason has to survive for diagnostics.
    unsigned long input_install_error{};
    bool input_installed{};
    HWND previous_capture{};
    RECT previous_cursor_clip{};
    bool cursor_clip_saved{};
    bool menu_cursor_active{};
    bool win32_initialized{};
    bool platform_ui_initialized{};
    bool plugin_ui_device_active{};
    bool lifecycle_invoker_bound{};
    bool plugins_loaded{};
    std::atomic<RendererLifecycle> renderer{RendererLifecycle::Cold};
    std::uint64_t selected_area{};
    cabbird::UiServiceRegistry ui_services;
    EmbeddedInputMailbox input_mailbox;
};

extern std::atomic<EmbeddedState*> g_state;
extern PresentFn g_present;
extern Present1Fn g_present1;
extern ResizeBuffersFn g_resize_buffers;
extern ResizeBuffers1Fn g_resize_buffers1;
inline constexpr std::string_view kRendererHookOwner = "cabbird.renderer";
inline constexpr std::uint64_t kRendererHookGeneration = 1;

[[nodiscard]] HookTargets DiscoverD3D11HookTargets();

[[nodiscard]] bool InstallEmbeddedInput(EmbeddedState& state) noexcept;
void RestoreEmbeddedInput(EmbeddedState& state) noexcept;
void RecordEmbeddedInputMessage(
    EmbeddedState& state, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// These are called on Render while the existing PluginManager mutex is held.
// The mailbox keeps their input collection independent from WndProc.
[[nodiscard]] bool PublishEmbeddedInputFrame(EmbeddedState& state) noexcept;
void PublishEmbeddedUiCapture(EmbeddedState& state) noexcept;

[[nodiscard]] const CabbirdUiServiceV1* EmbeddedUiServiceTable() noexcept;

void RenderEmbedded(IDXGISwapChain* swap_chain, UINT flags);
void HandlePresentResult(IDXGISwapChain* swap_chain, HRESULT result);
[[nodiscard]] bool ReleaseGraphics(
    EmbeddedState& state, RendererLifecycle final_state, bool force_release = false);

}  // namespace cabbird::embedded
