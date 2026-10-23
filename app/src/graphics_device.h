// narutorise - graphics card choice
//
// The graphics card by its name (graphics_device), for both renderers, as
// Unleashed Recompiled's GraphicsDevice option: empty keeps each renderer's own
// choice. The native renderer passes the name to Plume (gpu/backend/engine.cpp);
// the Xenos renderer takes the adapter index from it here, through the SDK's
// d3d12_adapter, while its D3D12 provider is created.

#pragma once

#ifdef _WIN32
// Before the Xenos plugin creates its D3D12 provider: d3d12_adapter set to the
// index of the named adapter, when it is present (graphics_device_win32.cpp).
void SelectXenosAdapter();

// After: d3d12_adapter back to its configured value, so that a settings save
// (F4) does not keep the index in the configuration.
void RestoreXenosAdapter();
#else
// Outside Windows there is no Xenos plugin.
inline void SelectXenosAdapter() {}
inline void RestoreXenosAdapter() {}
#endif
