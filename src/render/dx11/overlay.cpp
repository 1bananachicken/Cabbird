#include "overlay.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdio>

// These four were spelled as relative paths into a vendored third_party/ tree
// ("../../third_party/imgui/imgui.h").  Nothing is vendored any more: imgui and MinHook
// are fetched by tag at configure time, so the includes are the upstream spellings and
// the search paths come from the build -- "${imgui_SOURCE_DIR}" and its backends
// directory on cabbird_core, and MinHook's PUBLIC include directory through the fetched
// `minhook` target.  A path that names a directory the repository does not contain is
// exactly the kind of include that survives until someone else builds the tree.
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include "log.hpp"
#include "status.hpp"

#include "cabbird/cabbird_ui_theme.hpp"

// Implemented in the Win32 backend header, but that header only declares it when
// included inside the implementation unit, so declare it explicitly here.
#ifndef CABBIRD_IMGUI_NO_WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);
#endif

namespace cabbird {
namespace {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width,
                                         UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);

Options g_options;
std::atomic<bool> g_started{false};
std::atomic<bool> g_should_stop{false};

// Hook state ---------------------------------------------------------------
PresentFn g_original_present = nullptr;
ResizeBuffersFn g_original_resize = nullptr;
std::atomic<bool> g_hook_installed{false};

// ImGui / render state -----------------------------------------------------
std::atomic<bool> g_imgui_ready{false};
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
HWND g_window = nullptr;
WNDPROC g_original_wndproc = nullptr;
std::atomic<bool> g_pending_resize{false};
std::atomic<bool> g_swallow_mouse{false};
std::atomic<bool> g_swallow_keyboard{false};
bool g_logged_first_frame = false;
std::atomic<unsigned long long> g_frame_index{0};
std::atomic<bool> g_warned_not_d3d11{false};
// Set by the manual-map entry point; null in the proxy build.
FrameCallback g_frame_callback = nullptr;
// The shell stage and the real game stage use different swapchains.  Unless the
// render target is rebuilt for the new one, we would draw into a stale view.
std::atomic<void*> g_current_swap_chain{nullptr};

HMODULE g_minhook_module = nullptr;

// ---------------------------------------------------------------------------
// Window procedure interception
// ---------------------------------------------------------------------------

// Only these messages are ever swallowed, and only while ImGui reports it wants
// the corresponding input.  Window-management messages always reach the game so
// the overlay can never break focus, sizing or shutdown.
bool IsSwallowableInputMessage(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_INPUT:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_SIZE) {
        g_pending_resize.store(true, std::memory_order_relaxed);
    }

    if (g_imgui_ready.load(std::memory_order_acquire) && ImGui::GetCurrentContext() != nullptr) {
#ifndef CABBIRD_IMGUI_NO_WIN32
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
#endif

        const ImGuiIO& io = ImGui::GetIO();
        g_swallow_mouse.store(io.WantCaptureMouse, std::memory_order_relaxed);
        g_swallow_keyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);

        if (IsSwallowableInputMessage(msg)) {
            const bool wants =
                (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN ||
                 msg == WM_SYSKEYUP || msg == WM_INPUT)
                    ? g_swallow_keyboard.load(std::memory_order_relaxed)
                    : g_swallow_mouse.load(std::memory_order_relaxed);
            if (wants) {
                return 0;
            }
        }
    }

    if (g_original_wndproc != nullptr) {
        return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void InstallWndProcHook() {
    if (!g_options.wndproc_hook || g_window == nullptr) {
        return;
    }
    g_original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
    CABBIRD_LOG_INFO("wndproc: subclassed hwnd=0x%p previous=0x%p", g_window,
                 reinterpret_cast<void*>(g_original_wndproc));
}

void RemoveWndProcHook() {
    if (g_original_wndproc != nullptr && g_window != nullptr) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        g_original_wndproc = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ImGui lifecycle
// ---------------------------------------------------------------------------

void ReleaseRenderTarget() {
    if (g_render_target != nullptr) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

void CreateRenderTarget(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&back_buffer))) ||
        back_buffer == nullptr) {
        CABBIRD_LOG_ERROR("render target: GetBuffer(0) failed");
        return;
    }
    const HRESULT hr = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    if (FAILED(hr)) {
        CABBIRD_LOG_ERROR("render target: CreateRenderTargetView failed hr=0x%08lX",
                      static_cast<unsigned long>(hr));
        g_render_target = nullptr;
    }
}

bool EnsureImGui(IDXGISwapChain* swap_chain) {
    if (g_imgui_ready.load(std::memory_order_acquire)) {
        return true;
    }

    // Which graphics API owns this swapchain?  A D3D12 swapchain fails this QI.
    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
        if (!g_warned_not_d3d11.exchange(true)) {
            DXGI_SWAP_CHAIN_DESC desc{};
            swap_chain->GetDesc(&desc);
            StatusSet(L"swapchain_backend", "not_d3d11");
            CABBIRD_LOG_ERROR(
                "swapchain is NOT D3D11 (likely D3D12/Vulkan): format=%d buffers=%u windowed=%d "
                "hwnd=0x%p -- overlay cannot attach on this backend",
                static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
                desc.Windowed ? 1 : 0, desc.OutputWindow);
        }
        return false;
    }
    g_device = device;

    // GetDesc() gives us the real game window without enumerating windows.
    DXGI_SWAP_CHAIN_DESC desc{};
    swap_chain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    device->GetImmediateContext(&g_context);
    if (g_context == nullptr) {
        CABBIRD_LOG_ERROR("imgui: GetImmediateContext returned null");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // never write imgui.ini into the game directory
    io.LogFilename = nullptr;

    // The product's look, ported from Anomaly rather than re-invented.  This replaces
    // StyleColorsDark(): the default style is visibly a different product (different
    // rounding, padding and accent), which is exactly what "keep the UI consistent"
    // rules out.  The font atlas has to be configured before the backend creates its
    // device objects, and it needs the runtime root because a manually mapped image
    // cannot compute its own path -- StartOverlayHost() supplies that from Options.
    if (!cabbird::ConfigureCabbirdUiFontAtlas()) {
        // Logged at INFO with an explicit prefix rather than at a new WARN level, on
        // purpose: the ini numbering is 0=off 1=error 2=info 3=debug, so inserting a
        // level would silently re-map an existing `level=2` from info to warn.  A
        // renumbered severity is a config change disguised as a logging change.
        //
        // Not fatal, but worth saying out loud: a missing CJK font turns every Chinese
        // label into a '?' box, and that is otherwise easy to mistake for an encoding bug.
        CABBIRD_LOG_INFO("warning: imgui font atlas incomplete (missing CJK/icon font?) -- "
                         "localized text may render as missing glyphs");
    }
    cabbird::ApplyCabbirdUiStyle();

#ifndef CABBIRD_IMGUI_NO_WIN32
    if (!ImGui_ImplWin32_Init(g_window)) {
        CABBIRD_LOG_ERROR("imgui: ImGui_ImplWin32_Init failed (hwnd=0x%p)", g_window);
        ImGui::DestroyContext();
        return false;
    }
#endif
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        CABBIRD_LOG_ERROR("imgui: ImGui_ImplDX11_Init failed");
#ifndef CABBIRD_IMGUI_NO_WIN32
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        return false;
    }

    CreateRenderTarget(swap_chain);
    InstallWndProcHook();

    CABBIRD_LOG_INFO("imgui: ready (hwnd=0x%p buffers=%u format=%d windowed=%d)", g_window,
                 desc.BufferCount, static_cast<int>(desc.BufferDesc.Format), desc.Windowed ? 1 : 0);
    StatusSet(L"swapchain_backend", "d3d11");
    StatusSetInt(L"imgui_ready", 1);
    g_imgui_ready.store(true, std::memory_order_release);
    return true;
}

void DrawOverlayFrame() {
    ImGui_ImplDX11_NewFrame();
#ifdef CABBIRD_IMGUI_NO_WIN32
    // Manual-map builds must not link imgui_impl_win32.cpp: it references the
    // CRT's `_tls_index`, which pulls LIBCMT's tlssup.obj into the link and
    // gives the image a static-TLS directory that the manual mapper refuses.
    // A *blank* overlay needs no input, so the only
    // thing the Win32 backend was really providing is the display size and a
    // delta time -- both of which we can supply here.
    {
        ImGuiIO& io = ImGui::GetIO();
        auto* swap_chain =
            static_cast<IDXGISwapChain*>(g_current_swap_chain.load(std::memory_order_relaxed));
        if (swap_chain != nullptr) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(swap_chain->GetDesc(&desc)) && desc.BufferDesc.Width != 0) {
                io.DisplaySize =
                    ImVec2(static_cast<float>(desc.BufferDesc.Width),
                           static_cast<float>(desc.BufferDesc.Height));
            }
        }
        static ULONGLONG last_tick = 0;
        const ULONGLONG now = GetTickCount64();
        if (last_tick != 0) {
            io.DeltaTime = static_cast<float>(now - last_tick) / 1000.0f;
        }
        last_tick = now;
    }
#else
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();

    // The acceptance criterion for this probe is deliberately the *blankest*
    // possible overlay: one empty window, a title and a frame counter.  No
    // widgets, no demo window, no config UI.
    ImGui::SetNextWindowSize(ImVec2(430.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Cabbird :: overlay");
    if (g_options.show_status_text) {
        ImGui::Text("blank overlay is rendering");
        ImGui::Separator();
        ImGui::Text("frame %llu   %.1f FPS", g_frame_index.load(std::memory_order_relaxed),
                    static_cast<double>(ImGui::GetIO().Framerate));
    }
    ImGui::End();

    ImGui::Render();

    if (g_render_target == nullptr) {
        return;
    }
    g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT WINAPI PresentDetour(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (!g_should_stop.load(std::memory_order_relaxed)) {
        if (EnsureImGui(swap_chain)) {
            // A swapchain swap means the render target belongs to a dead
            // backbuffer; rebuild it before drawing.
            void* previous = g_current_swap_chain.exchange(swap_chain, std::memory_order_relaxed);
            if (previous != nullptr && previous != swap_chain) {
                CABBIRD_LOG_INFO("present: swapchain changed 0x%p -> 0x%p, rebuilding render target",
                             previous, swap_chain);
                ReleaseRenderTarget();
                LogFlush();
            }

            const unsigned long long frame =
                g_frame_index.fetch_add(1, std::memory_order_relaxed) + 1;

            // Optional consumer (the manual-map IL2CPP probe).  Deliberately
            // called before anything else here so that "the game is finally
            // rendering" is the trigger, and idle until the day it is enabled.
            if (const FrameCallback callback = g_frame_callback) {
                callback(frame);
            }

            if (!g_logged_first_frame) {
                g_logged_first_frame = true;
                StatusSetInt(L"first_frame_rendered", 1);
                CABBIRD_LOG_INFO("present: first hooked frame rendered (swapchain=0x%p)", swap_chain);
                LogFlush();
            }
            if (g_pending_resize.exchange(false, std::memory_order_relaxed)) {
                ReleaseRenderTarget();
            }
            if (g_render_target == nullptr) {
                CreateRenderTarget(swap_chain);
            }
            DrawOverlayFrame();

            // Periodic progress markers.  Without these, a kill that happens
            // seconds after the first frame is indistinguishable from our own
            // render thread having stalled -- the log would simply stop either
            // way.  These lines make "the game kept presenting until T" and
            // "the render thread stopped at T" two different, readable outcomes.
            if ((frame % 300) == 0) {
                CABBIRD_LOG_INFO("present: frame %llu (%.1f FPS)", frame,
                             static_cast<double>(ImGui::GetIO().Framerate));
                LogFlush();
            }
        }
    }
    return g_original_present(swap_chain, sync_interval, flags);
}

HRESULT WINAPI ResizeBuffersDetour(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width,
                                   UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    ReleaseRenderTarget();
    const HRESULT hr =
        g_original_resize(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    CABBIRD_LOG_DEBUG("resize_buffers: %ux%u -> hr=0x%08lX", width, height,
                  static_cast<unsigned long>(hr));
    return hr;
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Creates a hidden window + D3D11 swapchain purely to read the vtable slot.
bool CaptureSwapChainVtable(void** present_out, void** resize_out) {
    const wchar_t* class_name = L"CabbirdDummyWindow";
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DummyWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CABBIRD_LOG_ERROR("dummy: RegisterClassExW failed err=%lu", GetLastError());
        return false;
    }

    // Never shown: an off-screen window is enough for DXGI to hand out a vtable.
    HWND hwnd = CreateWindowExW(0, class_name, L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr,
                                instance, nullptr);
    if (hwnd == nullptr) {
        CABBIRD_LOG_ERROR("dummy: CreateWindowExW failed err=%lu", GetLastError());
        UnregisterClassW(class_name, instance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
    if (FAILED(hr) || swap_chain == nullptr) {
        CABBIRD_LOG_ERROR("dummy: D3D11CreateDeviceAndSwapChain failed hr=0x%08lX",
                      static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        UnregisterClassW(class_name, instance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swap_chain);
    *present_out = vtable[8];   // IDXGISwapChain::Present
    *resize_out = vtable[13];   // IDXGISwapChain::ResizeBuffers

    context->Release();
    device->Release();
    swap_chain->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(class_name, instance);

    CABBIRD_LOG_DEBUG("dummy: vtable present=0x%p resize=0x%p feature_level=0x%04X", *present_out,
                  *resize_out, static_cast<unsigned>(level));
    return true;
}

const char* PresentHookStatusText(MH_STATUS status) {
    switch (status) {
        case MH_OK:
            return "MH_OK";
        case MH_ERROR_ALREADY_INITIALIZED:
            return "MH_ERROR_ALREADY_INITIALIZED";
        case MH_ERROR_NOT_INITIALIZED:
            return "MH_ERROR_NOT_INITIALIZED";
        case MH_ERROR_ALREADY_CREATED:
            return "MH_ERROR_ALREADY_CREATED";
        case MH_ERROR_NOT_CREATED:
            return "MH_ERROR_NOT_CREATED";
        case MH_ERROR_ENABLED:
            return "MH_ERROR_ENABLED";
        case MH_ERROR_DISABLED:
            return "MH_ERROR_DISABLED";
        case MH_ERROR_NOT_EXECUTABLE:
            return "MH_ERROR_NOT_EXECUTABLE";
        case MH_ERROR_UNSUPPORTED_FUNCTION:
            return "MH_ERROR_UNSUPPORTED_FUNCTION";
        case MH_ERROR_MEMORY_ALLOC:
            return "MH_ERROR_MEMORY_ALLOC";
        case MH_ERROR_MEMORY_PROTECT:
            return "MH_ERROR_MEMORY_PROTECT";
        case MH_ERROR_MODULE_NOT_FOUND:
            return "MH_ERROR_MODULE_NOT_FOUND";
        case MH_ERROR_FUNCTION_NOT_FOUND:
            return "MH_ERROR_FUNCTION_NOT_FOUND";
        default:
            return "MH_ERROR_<unknown>";
    }
}

bool InstallPresentHook() {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        CABBIRD_LOG_ERROR("minhook: MH_Initialize failed (%s)", PresentHookStatusText(init));
        return false;
    }

    void* present = nullptr;
    void* resize = nullptr;
    if (!CaptureSwapChainVtable(&present, &resize)) {
        return false;
    }

    MH_STATUS status = MH_CreateHook(present, reinterpret_cast<void*>(&PresentDetour),
                                     reinterpret_cast<void**>(&g_original_present));
    if (status != MH_OK) {
        CABBIRD_LOG_ERROR("minhook: CreateHook(Present) failed (%s)", PresentHookStatusText(status));
        return false;
    }
    status = MH_EnableHook(present);
    if (status != MH_OK) {
        CABBIRD_LOG_ERROR("minhook: EnableHook(Present) failed (%s)", PresentHookStatusText(status));
        return false;
    }
    CABBIRD_LOG_INFO("minhook: Present hooked at %p", present);

    MH_STATUS resize_status = MH_CreateHook(resize, reinterpret_cast<void*>(&ResizeBuffersDetour),
                                            reinterpret_cast<void**>(&g_original_resize));
    if (resize_status == MH_OK) {
        resize_status = MH_EnableHook(resize);
    }
    if (resize_status != MH_OK) {
        // ResizeBuffers is a nicety (avoids a stale RTV); not fatal.
        CABBIRD_LOG_ERROR("minhook: ResizeBuffers hook unavailable (%s), continuing",
                      PresentHookStatusText(resize_status));
        g_original_resize = nullptr;
    } else {
        CABBIRD_LOG_INFO("minhook: ResizeBuffers hooked at %p", resize);
    }

    g_hook_installed.store(true);
    LogFlush();
    return true;
}

// Independent liveness beacon.  Logs on its own thread so that "our host thread
// is still alive but no frames are arriving" is distinguishable from "the whole
// process is gone".  Also mirrors counters into the status file, because that
// file survives a hard kill whereas buffered log lines may not.
DWORD WINAPI HeartbeatThread(LPVOID) {
    const ULONGLONG start = GetTickCount64();
    while (!g_should_stop.load(std::memory_order_relaxed)) {
        Sleep(5000);
        if (g_should_stop.load(std::memory_order_relaxed)) {
            break;
        }
        const long long uptime = static_cast<long long>((GetTickCount64() - start) / 1000ULL);
        const unsigned long long frames = g_frame_index.load(std::memory_order_relaxed);
        CABBIRD_LOG_INFO("heartbeat: uptime=%llds frames=%llu imgui=%d hooks=%d", uptime, frames,
                     g_imgui_ready.load(std::memory_order_relaxed) ? 1 : 0,
                     g_hook_installed.load(std::memory_order_relaxed) ? 1 : 0);
        StatusSetInt(L"uptime_s", uptime);
        StatusSetInt(L"frames_rendered", static_cast<long long>(frames));
        LogFlush();
    }
    return 0;
}

DWORD WINAPI OverlayHostThread(LPVOID) {
    CABBIRD_LOG_INFO("host: worker thread started (delay=%dms)", g_options.init_delay_ms);
    Sleep(static_cast<DWORD>(g_options.init_delay_ms));

    if (g_should_stop.load(std::memory_order_relaxed)) {
        return 0;
    }

    if (!g_options.enable_hooks) {
        CABBIRD_LOG_INFO("host: level-0 probe mode -- hooks and overlay disabled by config");
        LogFlush();
        return 0;
    }

    int attempt = 0;
    while (!g_should_stop.load(std::memory_order_relaxed)) {
        ++attempt;
        CABBIRD_LOG_INFO("host: installing Present hook (attempt %d)", attempt);
        if (InstallPresentHook()) {
            CABBIRD_LOG_INFO("host: hook installed; waiting for the game's first frame");
            LogFlush();
            return 0;
        }
        if (g_options.retry_attempts != 0 && attempt >= g_options.retry_attempts) {
            CABBIRD_LOG_ERROR("host: giving up after %d attempts", attempt);
            LogFlush();
            return 0;
        }
        Sleep(static_cast<DWORD>(g_options.retry_interval_ms));
    }
    return 0;
}

}  // namespace

void SetFrameCallback(FrameCallback callback) {
    g_frame_callback = callback;
}

bool StartOverlayHost(const Options& options) {
    if (g_started.exchange(true)) {
        return false;
    }
    g_options = options;
    g_should_stop.store(false);

    // A manually mapped image has no on-disk module path, so the UI cannot find its own
    // assets/fonts/ directory.  The injector already resolved the image's directory into
    // Options.dll_dir (it is the same directory it delivered the ini from), so pass it
    // along rather than re-deriving it here and getting a different answer.
    cabbird::SetCabbirdUiRuntimeRoot(options.dll_dir);

    HANDLE thread = CreateThread(nullptr, 0, &OverlayHostThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        CABBIRD_LOG_ERROR("host: CreateThread failed err=%lu", GetLastError());
        return false;
    }
    CloseHandle(thread);  // detached; the thread self-terminates

    HANDLE beat = CreateThread(nullptr, 0, &HeartbeatThread, nullptr, 0, nullptr);
    if (beat == nullptr) {
        CABBIRD_LOG_ERROR("host: heartbeat CreateThread failed err=%lu", GetLastError());
    } else {
        CloseHandle(beat);
    }
    return true;
}

void StopOverlayHost() {
    g_should_stop.store(true);
    if (g_hook_installed.load(std::memory_order_acquire)) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hook_installed.store(false);
    }
    if (g_imgui_ready.load(std::memory_order_acquire)) {
        RemoveWndProcHook();
        ImGui_ImplDX11_Shutdown();
#ifndef CABBIRD_IMGUI_NO_WIN32
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        ReleaseRenderTarget();
        if (g_context != nullptr) {
            g_context->Release();
            g_context = nullptr;
        }
        if (g_device != nullptr) {
            g_device->Release();
            g_device = nullptr;
        }
        g_imgui_ready.store(false);
    }
    LogFlush();
}

}  // namespace cabbird
