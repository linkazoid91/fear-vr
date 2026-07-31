#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d9.h>

namespace fearvr {

// Opt-in factory used by the proxy's Direct3DCreate9 export. The returned
// object preserves the classic IDirect3D9 ABI, while CreateDevice is backed by
// IDirect3D9Ex::CreateDeviceEx. Managed-resource compatibility is layered onto
// the returned device separately; callers must not enable this for Retail
// until that layer has passed its resource/reset tests.
IDirect3D9* CreateD3D9ExCompatibility(UINT sdkVersion) noexcept;

// D3D9Ex exclusive fullscreen is minimized when the OpenXR compositor takes
// focus on Retail. Preserve the requested render size and MSAA settings, but
// use a centered borderless window so the backbuffer remains available for
// the VR bridge. Returns true only when a fullscreen request was translated.
bool TranslateD3D9ExPresentation(
    HWND focusWindow, D3DPRESENT_PARAMETERS* parameters) noexcept;

bool D3D9ExCompatibilityRequested() noexcept;
bool D3D9ExExclusiveRequested() noexcept;
bool D3D9ExWindowedTestRequested() noexcept;

} // namespace fearvr
