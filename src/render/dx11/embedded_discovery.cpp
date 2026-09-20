#include "cabbird/unitymem_compat.hpp"
#include "embedded_host_internal.hpp"

namespace cabbird::embedded {
namespace {

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

// PROVISIONAL: this is the first Cabbird-authored function in the ported tree.
//
// The ported version asked D3D12CreateDevice for a device and a direct command queue
// purely to obtain three COM vtables, then read ExecuteCommandLists out of the queue's
// table alongside Present/ResizeBuffers out of the swap chain's.  Direct3D11 has no
// command queue and no ExecuteCommandLists, so that half cannot be ported -- and because
// HookTargets::operator bool counted execute_command_lists as required, keeping it would
// have made hook installation fail on this title.
//
// The replacement asks for a Direct3D11 device and a plain IDXGISwapChain instead.  The
// vtable slot numbers are the ones the ported code already used and were not re-derived:
// IDXGISwapChain inherits IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1), which
// puts Present at 8 and ResizeBuffers at 13 -- exactly the indices the D3D12 probe read
// from its own swap chain.  IDXGISwapChain1 adds Present1 at 22; IDXGISwapChain3 adds
// ResizeBuffers1 at 39 and needs QueryInterface because CreateSwapChain returns the base
// interface.
//
// The window class and window exist only to satisfy CreateSwapChain; both are torn down
// before returning.
HookTargets DiscoverD3D11HookTargets() {
    HookTargets targets;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"CabbirdD3D11HookProbe";
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, DummyWindowProc, 0, 0, instance, nullptr, nullptr,
        nullptr, nullptr, class_name, nullptr};
    const ATOM atom = RegisterClassExW(&window_class);
    HWND window = CreateWindowExW(
        0, class_name, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, instance, nullptr);

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 100;
    description.BufferDesc.Height = 100;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGIFactory1* factory{};
    IDXGISwapChain* swap_chain{};
    IDXGISwapChain1* swap_chain1{};
    IDXGISwapChain3* swap_chain3{};

    D3D_FEATURE_LEVEL level{};
    bool ready = window != nullptr && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    if (ready) {
        ID3D11Device* device{};
        ID3D11DeviceContext* context{};
        ready = SUCCEEDED(D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &device, &level, &context)) &&
            device != nullptr;
        if (ready) {
            ready = SUCCEEDED(factory->CreateSwapChain(device, &description, &swap_chain));
        }
        Release(context);
        Release(device);
    }
    if (ready) {
        ready = SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain1))) &&
            SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain3)));
    }
    if (ready) {
        auto** swap_table = *reinterpret_cast<void***>(swap_chain);
        auto** swap1_table = *reinterpret_cast<void***>(swap_chain1);
        auto** swap3_table = *reinterpret_cast<void***>(swap_chain3);
        targets.present = swap_table[8];
        targets.resize_buffers = swap_table[13];
        targets.present1 = swap1_table[22];
        targets.resize_buffers1 = swap3_table[39];
        // Deliberately left null: Direct3D11 has no ExecuteCommandLists to hook.  A
        // consumer that requires it must treat a D3D11 target as fully discovered.
        targets.execute_command_lists = nullptr;
    }
    Release(swap_chain3);
    Release(swap_chain1);
    Release(swap_chain);
    Release(factory);
    if (window != nullptr) DestroyWindow(window);
    if (atom != 0) UnregisterClassW(class_name, instance);
    return targets;
}

}  // namespace cabbird::embedded