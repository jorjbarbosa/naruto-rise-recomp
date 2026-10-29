// Attaching to a window that another flip-model swap chain still holds, as an
// earlier engine's swap chain can for a while after its release. While the
// window stays held, the attach fails with the HRESULT of the creation instead
// of crashing; when the other swap chain goes during the retries, the attach
// succeeds and the window presents.
#include "gpu/backend/engine.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace gpu;
using Microsoft::WRL::ComPtr;

namespace {
// A swap chain of a device of its own on the window.
struct Holder {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain1> swapchain;
  HRESULT Hold(HWND window) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapchain);
    return hr;
  }
  void Release() {
    swapchain.Reset();
    queue.Reset();
    device.Reset();
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto check = [&](const char* kind, bool ok, const std::string& evidence) {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
    return ok;
  };
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"NativeAttachTest";
  RegisterClassW(&cls);
  // Held: the other swap chain stays for the whole attach.
  // Released: it goes 300 ms into the attach, on another thread.
  for (bool released : {false, true}) {
    HWND window = CreateWindowW(cls.lpszClassName, L"Attach test", WS_POPUP, 0, 0, 64, 36, nullptr, nullptr,
                                cls.hInstance, nullptr);
    if (!check("window", window != nullptr, "{}")) return 4;
    Holder holder;
    const HRESULT held = holder.Hold(window);
    if (!check("hold", SUCCEEDED(held), "{\"hresult\":" + std::to_string(uint32_t(held)) + "}")) return 5;
    Engine engine;
    std::string evidence;
    if (!check("initialize", engine.Initialize(evidence), evidence)) return 6;
    std::thread release;
    if (released) release = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      holder.Release();
    });
    const bool attached = engine.AttachWindow(platform::WindowHandles(window), evidence);
    if (release.joinable()) release.join();
    // E_ACCESSDENIED: the window already has a flip-model swap chain.
    const bool access_denied = evidence.find("\"hresult\":2147942405") != std::string::npos ||
                               evidence.find("\"create_hresult\":2147942405") != std::string::npos;
    check(released ? "attach-after-release" : "attach-held", attached == released && access_denied, evidence);
    if (attached) {
      SurfaceDesc surface{0x1234, 64, 36};
      check("surface", engine.CreateSurface(surface, evidence), evidence);
      ClearRequest clear;
      clear.color_keys[0] = surface.key;
      clear.clear_color = true;
      clear.color = {0, 1, 0, 1};
      uint64_t fence = 0;
      check("clear", engine.ClearSurfaces(clear, fence, evidence), evidence);
      uint32_t slot = 0, width = 0, height = 0;
      std::vector<uint8_t> pixels;
      check("stage", engine.StageFront(surface.key, slot, evidence), evidence);
      const bool presented = engine.Present(slot, true, pixels, width, height, evidence);
      check("present", presented && pixels.size() == size_t(width) * height * 4 && pixels.size() >= 4 &&
                           pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0, evidence);
      check("release", engine.ReleaseRemaining(evidence), evidence);
      check("detach", engine.DetachWindow(evidence), evidence);
    }
    check("shutdown", engine.Shutdown(evidence), evidence);
    holder.Release();
    DestroyWindow(window);
  }
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
