#include "d3d9ex_compat.h"

#include "bridge.h"
#include "system_d3d9.h"

#include <algorithm>
#include <cstdio>
#include <new>

#include <shellapi.h>
#include <wrl/client.h>

namespace fearvr {
namespace {

using Microsoft::WRL::ComPtr;

class D3D9ExCompatibility final : public IDirect3D9 {
public:
    explicit D3D9ExCompatibility(IDirect3D9Ex* inner) noexcept
        : inner_(inner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interfaceId, void** output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (interfaceId == __uuidof(IUnknown) ||
            interfaceId == __uuidof(IDirect3D9)) {
            *output = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        // Direct3DCreate9 returns a classic factory. Exposing the inner
        // IDirect3D9Ex object would break COM identity and bypass this
        // compatibility layer's CreateDevice translation.
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(
        void* initializeFunction) override {
        return inner_->RegisterSoftwareDevice(initializeFunction);
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return inner_->GetAdapterCount();
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(
        UINT adapter, DWORD flags,
        D3DADAPTER_IDENTIFIER9* identifier) override {
        return inner_->GetAdapterIdentifier(adapter, flags, identifier);
    }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(
        UINT adapter, D3DFORMAT format) override {
        return inner_->GetAdapterModeCount(adapter, format);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(
        UINT adapter, D3DFORMAT format, UINT mode,
        D3DDISPLAYMODE* displayMode) override {
        return inner_->EnumAdapterModes(
            adapter, format, mode, displayMode);
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(
        UINT adapter, D3DDISPLAYMODE* displayMode) override {
        return inner_->GetAdapterDisplayMode(adapter, displayMode);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DFORMAT adapterFormat, D3DFORMAT backBufferFormat,
        BOOL windowed) override {
        return inner_->CheckDeviceType(
            adapter, deviceType, adapterFormat, backBufferFormat,
            windowed);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DFORMAT adapterFormat, DWORD usage,
        D3DRESOURCETYPE resourceType,
        D3DFORMAT checkFormat) override {
        return inner_->CheckDeviceFormat(
            adapter, deviceType, adapterFormat, usage, resourceType,
            checkFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DFORMAT surfaceFormat, BOOL windowed,
        D3DMULTISAMPLE_TYPE multiSampleType,
        DWORD* qualityLevels) override {
        return inner_->CheckDeviceMultiSampleType(
            adapter, deviceType, surfaceFormat, windowed,
            multiSampleType, qualityLevels);
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat,
        D3DFORMAT depthStencilFormat) override {
        return inner_->CheckDepthStencilMatch(
            adapter, deviceType, adapterFormat, renderTargetFormat,
            depthStencilFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DFORMAT sourceFormat,
        D3DFORMAT targetFormat) override {
        return inner_->CheckDeviceFormatConversion(
            adapter, deviceType, sourceFormat, targetFormat);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(
        UINT adapter, D3DDEVTYPE deviceType,
        D3DCAPS9* caps) override {
        return inner_->GetDeviceCaps(adapter, deviceType, caps);
    }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
        return inner_->GetAdapterMonitor(adapter);
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
        DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* presentationParameters,
        IDirect3DDevice9** output) override {
        if (presentationParameters == nullptr || output == nullptr) {
            return D3DERR_INVALIDCALL;
        }
        *output = nullptr;

        const bool requestedFullscreen =
            !presentationParameters->Windowed;
        const bool borderless =
            !D3D9ExExclusiveRequested() &&
            TranslateD3D9ExPresentation(
                focusWindow, presentationParameters);

        D3DDISPLAYMODEEX fullscreenMode{};
        D3DDISPLAYMODEEX* fullscreenModePointer = nullptr;
        if (!presentationParameters->Windowed) {
            const UINT requestedRefresh =
                presentationParameters->FullScreen_RefreshRateInHz;

            fullscreenMode.Size = sizeof(fullscreenMode);
            D3DDISPLAYROTATION rotation =
                D3DDISPLAYROTATION_IDENTITY;
            const HRESULT modeResult =
                inner_->GetAdapterDisplayModeEx(
                    adapter, &fullscreenMode, &rotation);
            if (FAILED(modeResult)) {
                fullscreenMode.Width =
                    presentationParameters->BackBufferWidth;
                fullscreenMode.Height =
                    presentationParameters->BackBufferHeight;
                fullscreenMode.RefreshRate =
                    presentationParameters->FullScreen_RefreshRateInHz;
                fullscreenMode.Format =
                    presentationParameters->BackBufferFormat;
                fullscreenMode.ScanLineOrdering =
                    D3DSCANLINEORDERING_PROGRESSIVE;
            } else {
                if (presentationParameters->BackBufferWidth != 0) {
                    fullscreenMode.Width =
                        presentationParameters->BackBufferWidth;
                }
                if (presentationParameters->BackBufferHeight != 0) {
                    fullscreenMode.Height =
                        presentationParameters->BackBufferHeight;
                }
                if (presentationParameters->BackBufferFormat !=
                    D3DFMT_UNKNOWN) {
                    fullscreenMode.Format =
                        presentationParameters->BackBufferFormat;
                }
                if (presentationParameters->
                        FullScreen_RefreshRateInHz != 0) {
                    fullscreenMode.RefreshRate =
                        presentationParameters->
                            FullScreen_RefreshRateInHz;
                }
            }

            // Classic D3D9 accepts refresh 0 in fullscreen as "choose a
            // valid default". CreateDeviceEx requires an enumerated refresh
            // in both structures. Pick the closest available mode to the
            // current desktop refresh while preserving Retail's resolution.
            D3DDISPLAYMODEFILTER filter{};
            filter.Size = sizeof(filter);
            filter.Format = fullscreenMode.Format;
            filter.ScanLineOrdering =
                D3DSCANLINEORDERING_PROGRESSIVE;
            const UINT modeCount =
                inner_->GetAdapterModeCountEx(adapter, &filter);
            D3DDISPLAYMODEEX selectedMode{};
            bool selected = false;
            UINT bestRefreshDelta = UINT_MAX;
            for (UINT modeIndex = 0;
                 modeIndex < modeCount; ++modeIndex) {
                D3DDISPLAYMODEEX candidate{};
                candidate.Size = sizeof(candidate);
                if (FAILED(inner_->EnumAdapterModesEx(
                        adapter, &filter, modeIndex, &candidate)) ||
                    candidate.Width != fullscreenMode.Width ||
                    candidate.Height != fullscreenMode.Height) {
                    continue;
                }
                if (requestedRefresh != 0 &&
                    candidate.RefreshRate != requestedRefresh) {
                    continue;
                }
                const UINT refreshDelta =
                    candidate.RefreshRate > fullscreenMode.RefreshRate
                        ? candidate.RefreshRate -
                            fullscreenMode.RefreshRate
                        : fullscreenMode.RefreshRate -
                            candidate.RefreshRate;
                if (!selected || refreshDelta < bestRefreshDelta) {
                    selectedMode = candidate;
                    selected = true;
                    bestRefreshDelta = refreshDelta;
                }
            }
            if (selected) {
                fullscreenMode = selectedMode;
            }

            // Keep the input/output presentation structure consistent with
            // pFullscreenDisplayMode. This is the key semantic translation
            // for Retail's zero-refresh classic D3D9 request.
            presentationParameters->BackBufferWidth =
                fullscreenMode.Width;
            presentationParameters->BackBufferHeight =
                fullscreenMode.Height;
            presentationParameters->BackBufferFormat =
                fullscreenMode.Format;
            presentationParameters->FullScreen_RefreshRateInHz =
                fullscreenMode.RefreshRate;
            fullscreenModePointer = &fullscreenMode;
        }

        IDirect3DDevice9Ex* deviceEx = nullptr;
        HRESULT result = inner_->CreateDeviceEx(
            adapter, deviceType, focusWindow, behaviorFlags,
            presentationParameters, fullscreenModePointer, &deviceEx);
        if (SUCCEEDED(result) && deviceEx == nullptr) {
            result = E_POINTER;
        }
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "HRESULT=0x%08lX requested_fullscreen=%d borderless=%d "
            "windowed=%d size=%ux%u format=%u "
            "refresh=%u swap=%u interval=%u behavior=0x%08lX",
            static_cast<unsigned long>(result),
            requestedFullscreen, borderless,
            presentationParameters->Windowed,
            presentationParameters->BackBufferWidth,
            presentationParameters->BackBufferHeight,
            static_cast<unsigned>(presentationParameters->BackBufferFormat),
            presentationParameters->FullScreen_RefreshRateInHz,
            static_cast<unsigned>(presentationParameters->SwapEffect),
            presentationParameters->PresentationInterval,
            static_cast<unsigned long>(behaviorFlags));
        ReportHookStatus(
            SUCCEEDED(result) ? "INFO" : "ERROR",
            "d3d9ex_compat_device", message);
        if (SUCCEEDED(result)) {
            *output = deviceEx;
        } else if (deviceEx != nullptr) {
            deviceEx->Release();
        }
        return result;
    }

private:
    ~D3D9ExCompatibility() = default;

    volatile LONG references_{1};
    ComPtr<IDirect3D9Ex> inner_;
};

} // namespace

bool TranslateD3D9ExPresentation(
    HWND focusWindow, D3DPRESENT_PARAMETERS* parameters) noexcept {
    if (parameters == nullptr || parameters->Windowed) {
        return false;
    }

    const UINT requestedWidth = parameters->BackBufferWidth;
    const UINT requestedHeight = parameters->BackBufferHeight;
    const bool smallWindow = D3D9ExWindowedTestRequested();
    parameters->Windowed = TRUE;
    parameters->FullScreen_RefreshRateInHz = 0;
    if (smallWindow) {
        parameters->BackBufferWidth = 960;
        parameters->BackBufferHeight = 540;
    }
    if (parameters->hDeviceWindow == nullptr) {
        parameters->hDeviceWindow = focusWindow;
    }

    HWND window = parameters->hDeviceWindow;
    if (window == nullptr) {
        ReportHookStatus(
            "WARN", "d3d9ex_borderless_window_missing",
            "Fullscreen was made nonexclusive, but no device window "
            "was available for borderless placement.");
        return true;
    }

    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (smallWindow) {
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    } else {
        style &= ~(static_cast<LONG_PTR>(
            WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_SYSMENU));
        style |= WS_POPUP | WS_VISIBLE;
    }
    SetWindowLongPtrW(window, GWL_STYLE, style);

    LONG_PTR extendedStyle =
        GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!smallWindow) {
        extendedStyle &= ~(static_cast<LONG_PTR>(
            WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
            WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    }
    SetWindowLongPtrW(window, GWL_EXSTYLE, extendedStyle);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(
        window, MONITOR_DEFAULTTONEAREST);
    const bool monitorKnown =
        monitor != nullptr &&
        GetMonitorInfoW(monitor, &monitorInfo) != FALSE;

    const LONG monitorWidth = monitorKnown
        ? monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left
        : static_cast<LONG>(requestedWidth);
    const LONG monitorHeight = monitorKnown
        ? monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top
        : static_cast<LONG>(requestedHeight);
    const LONG clientWidth = smallWindow
        ? static_cast<LONG>(parameters->BackBufferWidth)
        : requestedWidth != 0
        ? static_cast<LONG>(requestedWidth)
        : monitorWidth;
    const LONG clientHeight = smallWindow
        ? static_cast<LONG>(parameters->BackBufferHeight)
        : requestedHeight != 0
        ? static_cast<LONG>(requestedHeight)
        : monitorHeight;
    RECT windowRectangle{
        0, 0, clientWidth, clientHeight};
    if (smallWindow) {
        AdjustWindowRectEx(
            &windowRectangle, static_cast<DWORD>(style), FALSE,
            static_cast<DWORD>(extendedStyle));
    }
    const LONG width =
        windowRectangle.right - windowRectangle.left;
    const LONG height =
        windowRectangle.bottom - windowRectangle.top;
    const LONG left = (monitorKnown ? monitorInfo.rcMonitor.left : 0) +
        (std::max)(0L, (monitorWidth - width) / 2);
    const LONG top = (monitorKnown ? monitorInfo.rcMonitor.top : 0) +
        (std::max)(0L, (monitorHeight - height) / 2);

    SetWindowPos(
        window, HWND_TOP, left, top, width, height,
        SWP_FRAMECHANGED |
            (smallWindow ? 0 : SWP_NOACTIVATE) |
            SWP_SHOWWINDOW);

    char message[256]{};
    std::snprintf(
        message, sizeof(message),
        "requested=%ux%u backbuffer=%ux%u window=%ldx%ld "
        "position=%ld,%ld desktop_test=%d",
        requestedWidth, requestedHeight,
        parameters->BackBufferWidth,
        parameters->BackBufferHeight,
        width, height, left, top, smallWindow);
    ReportHookStatus(
        "INFO", "d3d9ex_borderless", message);
    return true;
}

bool CommandLineHasArgument(const wchar_t* expected) noexcept {
    int argumentCount = 0;
    wchar_t** arguments =
        CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }
    bool requested = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], expected) == 0) {
            requested = true;
            break;
        }
    }
    LocalFree(arguments);
    return requested;
}

bool D3D9ExCompatibilityRequested() noexcept {
    return CommandLineHasArgument(L"-fearvr-d3d9ex-compat");
}

bool D3D9ExExclusiveRequested() noexcept {
    return CommandLineHasArgument(L"-fearvr-d3d9ex-exclusive");
}

bool D3D9ExWindowedTestRequested() noexcept {
    return CommandLineHasArgument(
        L"-fearvr-d3d9ex-windowed-test");
}

IDirect3D9* CreateD3D9ExCompatibility(UINT sdkVersion) noexcept {
    using Function = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    const Function createDirect3DEx =
        ResolveSystemD3D9<Function>("Direct3DCreate9Ex");
    if (createDirect3DEx == nullptr) {
        return nullptr;
    }

    IDirect3D9Ex* direct3DEx = nullptr;
    const HRESULT result =
        createDirect3DEx(sdkVersion, &direct3DEx);
    if (FAILED(result) || direct3DEx == nullptr) {
        return nullptr;
    }

    if (!OnDirect3D9ExCreated(direct3DEx)) {
        direct3DEx->Release();
        return nullptr;
    }
    D3D9ExCompatibility* compatibility =
        new (std::nothrow) D3D9ExCompatibility(direct3DEx);
    direct3DEx->Release();
    return compatibility;
}

} // namespace fearvr
