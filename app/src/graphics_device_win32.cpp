// narutorise - graphics card choice on Windows (graphics_device.h): the Xenos
// renderer's adapter and the discrete GPU of hybrid laptops.

#include "graphics_device.h"

#include <optional>
#include <string>

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, graphics_device);

// The discrete GPU on hybrid laptops (NVIDIA Optimus, AMD PowerXpress), as
// Unleashed Recompiled does: the drivers read these exports from the
// executable only, not from rexruntime.dll, which also has them.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {
std::optional<std::string> configured_adapter;

std::string Utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(size_t(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}
}  // namespace

void SelectXenosAdapter() {
  const std::string name = REXCVAR_GET(graphics_device);
  if (name.empty() || !rex::cvar::GetFlagInfo("d3d12_adapter")) return;
  // The order of the SDK's provider (d3d12_provider.cpp): EnumAdapters1.
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK; ++index) {
    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
        Utf8(desc.Description) != name)
      continue;
    // The provider skips an adapter without Direct3D 12 and fails when the index names one.
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
      REXLOG_WARN("[narutorise] graphics_device \"{}\" has no Direct3D 12; the automatic choice is used", name);
      return;
    }
    configured_adapter = rex::cvar::GetFlagByName("d3d12_adapter");
    rex::cvar::SetFlagByName("d3d12_adapter", std::to_string(index));
    return;
  }
  REXLOG_WARN("[narutorise] graphics_device \"{}\" not found; the automatic choice is used", name);
}

void RestoreXenosAdapter() {
  if (!configured_adapter) return;
  rex::cvar::SetFlagByName("d3d12_adapter", *configured_adapter);
  configured_adapter.reset();
}
