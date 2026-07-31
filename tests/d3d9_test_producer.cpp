#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>

namespace {

struct Options {
    std::uint64_t frameLimit{900};
    std::uint64_t adapterLuid{0};
    bool compatibilitySmoke{false};
    bool classicD3D9{false};
    bool stereo{false};
    bool expectExDevice{false};
    bool probeManagedResources{false};
    bool fullscreen{false};
    bool fullscreenDefaultRefresh{false};
    bool multisample4{false};
    bool presentEx{false};
    bool swapChainPresent{false};
    bool sceneBoundary{false};
    bool offscreenFinal{false};
};

using BeginEyeFunction = void(__cdecl*)(std::uint32_t);
using CaptureEyeFunction = void(__cdecl*)(std::uint32_t);
using EndStereoFrameFunction = void(__cdecl*)(std::uint64_t);

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

Options ParseOptions(int argumentCount, char** arguments) {
    Options options;
    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        if (argument == "--compat-smoke") {
            options.compatibilitySmoke = true;
            options.classicD3D9 = true;
            options.expectExDevice = true;
            options.probeManagedResources = true;
            options.sceneBoundary = true;
            options.frameLimit = 120;
            continue;
        }
        if (argument == "--classic-d3d9") {
            options.classicD3D9 = true;
            continue;
        }
        if (argument == "--stereo") {
            options.stereo = true;
            continue;
        }
        if (argument == "--expect-ex-device") {
            options.expectExDevice = true;
            continue;
        }
        if (argument == "--probe-managed-resources") {
            options.probeManagedResources = true;
            continue;
        }
        if (argument == "--fullscreen") {
            options.fullscreen = true;
            continue;
        }
        if (argument == "--fullscreen-default-refresh") {
            options.fullscreen = true;
            options.fullscreenDefaultRefresh = true;
            continue;
        }
        if (argument == "--multisample4") {
            options.multisample4 = true;
            continue;
        }
        if (argument == "--present-ex") {
            options.presentEx = true;
            continue;
        }
        if (argument == "--swapchain-present") {
            options.swapChainPresent = true;
            continue;
        }
        if (argument == "--scene-boundary") {
            options.sceneBoundary = true;
            continue;
        }
        if (argument == "--offscreen-final") {
            options.offscreenFinal = true;
            continue;
        }
        if (index + 1 >= argumentCount) {
            continue;
        }
        const int radix = argument == "--adapter-luid" ? 0 : 10;
        if (argument != "--frames" && argument != "--adapter-luid") {
            continue;
        }
        char* end = nullptr;
        const unsigned long long parsed =
            std::strtoull(arguments[index + 1], &end, radix);
        if (end != arguments[index + 1] && *end == '\0' && parsed != 0) {
            if (argument == "--frames") {
                options.frameLimit = static_cast<std::uint64_t>(parsed);
            } else {
                options.adapterLuid =
                    static_cast<std::uint64_t>(parsed);
            }
        }
    }
    return options;
}

bool PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

D3DCOLOR FrameColor(std::uint64_t frame) {
    const auto red = static_cast<unsigned>((frame * 5U) & 0xFFU);
    const auto green = static_cast<unsigned>((frame * 3U + 85U) & 0xFFU);
    const auto blue = static_cast<unsigned>((frame * 7U + 170U) & 0xFFU);
    return D3DCOLOR_XRGB(red, green, blue);
}

D3DCOLOR StereoEyeColor(std::uint64_t frame, std::uint32_t eye) {
    const auto pulse = static_cast<unsigned>((frame * 3U) & 0x7FU);
    return eye == 0
        ? D3DCOLOR_XRGB(128U + pulse, 24U, 16U)
        : D3DCOLOR_XRGB(16U, 24U, 128U + pulse);
}

struct MarkerVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

struct IndexedMarker {
    IDirect3DVertexBuffer9* vertices{nullptr};
    IDirect3DIndexBuffer9* indices{nullptr};
};

void ReleaseIndexedMarker(IndexedMarker& marker) {
    if (marker.indices != nullptr) {
        marker.indices->Release();
        marker.indices = nullptr;
    }
    if (marker.vertices != nullptr) {
        marker.vertices->Release();
        marker.vertices = nullptr;
    }
}

HRESULT CreateIndexedMarker(
    IDirect3DDevice9* device, IndexedMarker& marker) {
    constexpr DWORD markerFvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    const MarkerVertex vertices[4] = {
        {15.5F, 15.5F, 0.0F, 1.0F, D3DCOLOR_XRGB(255, 255, 255)},
        {95.5F, 15.5F, 0.0F, 1.0F, D3DCOLOR_XRGB(255, 255, 255)},
        {15.5F, 95.5F, 0.0F, 1.0F, D3DCOLOR_XRGB(255, 255, 255)},
        {95.5F, 95.5F, 0.0F, 1.0F, D3DCOLOR_XRGB(255, 255, 255)}};
    const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};

    HRESULT result = device->CreateVertexBuffer(
        sizeof(vertices), D3DUSAGE_WRITEONLY, markerFvf,
        D3DPOOL_MANAGED, &marker.vertices, nullptr);
    void* destination = nullptr;
    if (SUCCEEDED(result)) {
        result = marker.vertices->Lock(
            0, sizeof(vertices), &destination, 0);
    }
    if (SUCCEEDED(result)) {
        std::memcpy(destination, vertices, sizeof(vertices));
        result = marker.vertices->Unlock();
    }
    if (SUCCEEDED(result)) {
        result = device->CreateIndexBuffer(
            sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
            D3DPOOL_MANAGED, &marker.indices, nullptr);
    }
    if (SUCCEEDED(result)) {
        result = marker.indices->Lock(
            0, sizeof(indices), &destination, 0);
    }
    if (SUCCEEDED(result)) {
        std::memcpy(destination, indices, sizeof(indices));
        result = marker.indices->Unlock();
    }
    if (FAILED(result)) {
        ReleaseIndexedMarker(marker);
    }
    return result;
}

HRESULT DrawIndexedMarker(
    IDirect3DDevice9* device, const IndexedMarker& marker) {
    constexpr DWORD markerFvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    HRESULT result = device->SetTexture(0, nullptr);
    if (SUCCEEDED(result)) {
        result = device->SetFVF(markerFvf);
    }
    if (SUCCEEDED(result)) {
        result = device->SetStreamSource(
            0, marker.vertices, 0, sizeof(MarkerVertex));
    }
    if (SUCCEEDED(result)) {
        result = device->SetIndices(marker.indices);
    }
    if (SUCCEEDED(result)) {
        result = device->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    }
    device->SetIndices(nullptr);
    device->SetStreamSource(0, nullptr, 0, 0);
    return result;
}

bool VerifyIndexedMarkerPixel(
    IDirect3DDevice9* device, const char* phase) {
    IDirect3DSurface9* renderTarget = nullptr;
    HRESULT result = device->GetRenderTarget(0, &renderTarget);
    D3DSURFACE_DESC description{};
    if (SUCCEEDED(result)) {
        result = renderTarget->GetDesc(&description);
    }

    IDirect3DSurface9* readback = nullptr;
    if (SUCCEEDED(result) &&
        description.MultiSampleType != D3DMULTISAMPLE_NONE) {
        result = D3DERR_INVALIDCALL;
    }
    if (SUCCEEDED(result)) {
        result = device->CreateOffscreenPlainSurface(
            description.Width, description.Height, description.Format,
            D3DPOOL_SYSTEMMEM, &readback, nullptr);
    }
    if (SUCCEEDED(result)) {
        result = device->GetRenderTargetData(renderTarget, readback);
    }

    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(result)) {
        result = readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    }
    std::uint32_t pixel = 0;
    if (SUCCEEDED(result) && locked.pBits != nullptr &&
        description.Width > 32 && description.Height > 32) {
        const auto* row = static_cast<const std::uint8_t*>(locked.pBits) +
            32U * static_cast<std::size_t>(locked.Pitch);
        std::memcpy(&pixel, row + 32U * sizeof(pixel), sizeof(pixel));
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }
    if (readback != nullptr && locked.pBits != nullptr) {
        readback->UnlockRect();
    }
    if (readback != nullptr) {
        readback->Release();
    }
    if (renderTarget != nullptr) {
        renderTarget->Release();
    }

    const bool white =
        ((pixel >> 16U) & 0xffU) >= 240U &&
        ((pixel >> 8U) & 0xffU) >= 240U &&
        (pixel & 0xffU) >= 240U;
    std::printf(
        "D3D9Ex compatibility pixel %s: HRESULT=0x%08lX "
        "pixel=0x%08lX %s\n",
        phase, static_cast<unsigned long>(result),
        static_cast<unsigned long>(pixel),
        SUCCEEDED(result) && white ? "PASS" : "FAIL");
    return SUCCEEDED(result) && white;
}

HRESULT DrawMarker(IDirect3DDevice9* device, D3DCOLOR color) {
    constexpr DWORD markerFvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    const MarkerVertex vertices[4] = {
        {15.5F, 15.5F, 0.0F, 1.0F, color},
        {79.5F, 15.5F, 0.0F, 1.0F, color},
        {15.5F, 79.5F, 0.0F, 1.0F, color},
        {79.5F, 79.5F, 0.0F, 1.0F, color}};

    IDirect3DVertexBuffer9* buffer = nullptr;
    HRESULT result = device->CreateVertexBuffer(
        sizeof(vertices), D3DUSAGE_WRITEONLY, markerFvf,
        D3DPOOL_DEFAULT, &buffer, nullptr);
    if (FAILED(result) || buffer == nullptr) {
        return result;
    }
    void* destination = nullptr;
    result = buffer->Lock(0, sizeof(vertices), &destination, 0);
    if (SUCCEEDED(result)) {
        std::memcpy(destination, vertices, sizeof(vertices));
        result = buffer->Unlock();
    }
    if (SUCCEEDED(result)) {
        result = device->SetTexture(0, nullptr);
    }
    if (SUCCEEDED(result)) {
        result = device->SetFVF(markerFvf);
    }
    if (SUCCEEDED(result)) {
        result = device->SetStreamSource(
            0, buffer, 0, sizeof(MarkerVertex));
    }
    if (SUCCEEDED(result)) {
        result = device->DrawPrimitive(
            D3DPT_TRIANGLESTRIP, 0, 2);
    }
    device->SetStreamSource(0, nullptr, 0, 0);
    buffer->Release();
    return result;
}

bool ProbeManagedResources(
    IDirect3DDevice9* device,
    IDirect3DTexture9** persistentTexture) {
    if (device == nullptr || persistentTexture == nullptr) {
        return false;
    }
    *persistentTexture = nullptr;

    IDirect3DTexture9* texture = nullptr;
    HRESULT result = device->CreateTexture(
        16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &texture, nullptr);
    if (FAILED(result) || texture == nullptr) {
        std::fprintf(
            stderr, "Managed texture failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        return false;
    }
    D3DLOCKED_RECT lockedRect{};
    result = texture->LockRect(0, &lockedRect, nullptr, 0);
    if (FAILED(result)) {
        std::fprintf(
            stderr, "Managed texture lock failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        texture->Release();
        return false;
    }
    std::memset(lockedRect.pBits, 0x5a, 16U * 4U);
    texture->UnlockRect(0);

    IDirect3DVolumeTexture9* volume = nullptr;
    result = device->CreateVolumeTexture(
        8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &volume, nullptr);
    D3DLOCKED_BOX lockedBox{};
    if (SUCCEEDED(result) && volume != nullptr) {
        result = volume->LockBox(0, &lockedBox, nullptr, 0);
    }
    if (FAILED(result) || volume == nullptr) {
        std::fprintf(
            stderr, "Managed volume failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        if (volume != nullptr) {
            volume->Release();
        }
        texture->Release();
        return false;
    }
    volume->UnlockBox(0);
    volume->Release();

    IDirect3DCubeTexture9* cube = nullptr;
    result = device->CreateCubeTexture(
        8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &cube, nullptr);
    if (SUCCEEDED(result) && cube != nullptr) {
        result = cube->LockRect(
            D3DCUBEMAP_FACE_POSITIVE_X, 0, &lockedRect, nullptr, 0);
    }
    if (FAILED(result) || cube == nullptr) {
        std::fprintf(
            stderr, "Managed cube failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        if (cube != nullptr) {
            cube->Release();
        }
        texture->Release();
        return false;
    }
    cube->UnlockRect(D3DCUBEMAP_FACE_POSITIVE_X, 0);
    cube->Release();

    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    result = device->CreateVertexBuffer(
        256, D3DUSAGE_WRITEONLY, D3DFVF_XYZ,
        D3DPOOL_MANAGED, &vertexBuffer, nullptr);
    void* bufferData = nullptr;
    if (SUCCEEDED(result) && vertexBuffer != nullptr) {
        result = vertexBuffer->Lock(0, 0, &bufferData, 0);
    }
    if (FAILED(result) || vertexBuffer == nullptr) {
        std::fprintf(
            stderr, "Managed vertex buffer failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        if (vertexBuffer != nullptr) {
            vertexBuffer->Release();
        }
        texture->Release();
        return false;
    }
    vertexBuffer->Unlock();
    vertexBuffer->Release();

    IDirect3DIndexBuffer9* indexBuffer = nullptr;
    std::printf("M2 managed index probe: create\n");
    result = device->CreateIndexBuffer(
        128, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
        D3DPOOL_MANAGED, &indexBuffer, nullptr);
    std::printf(
        "M2 managed index probe: create HRESULT=0x%08lX ptr=%p\n",
        static_cast<unsigned long>(result),
        static_cast<void*>(indexBuffer));
    if (SUCCEEDED(result) && indexBuffer != nullptr) {
        std::printf("M2 managed index probe: lock\n");
        result = indexBuffer->Lock(0, 0, &bufferData, 0);
    }
    if (FAILED(result) || indexBuffer == nullptr) {
        std::fprintf(
            stderr, "Managed index buffer failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        if (indexBuffer != nullptr) {
            indexBuffer->Release();
        }
        texture->Release();
        return false;
    }
    std::printf("M2 managed index probe: unlock\n");
    indexBuffer->Unlock();
    std::printf("M2 managed index probe: bind\n");
    result = device->SetIndices(indexBuffer);
    if (SUCCEEDED(result)) {
        result = device->SetIndices(nullptr);
    }
    if (FAILED(result)) {
        std::fprintf(
            stderr,
            "Managed index buffer bind failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(result));
        indexBuffer->Release();
        texture->Release();
        return false;
    }
    indexBuffer->Release();

    *persistentTexture = texture;
    std::printf("M2 managed resource probe: OK\n");
    return true;
}

HRESULT ResetDevice(IDirect3DDevice9* device,
                    D3DPRESENT_PARAMETERS& parameters, HWND window,
                    IDirect3DTexture9* persistentTexture) {
    constexpr LONG width = 800;
    constexpr LONG height = 450;
    SetWindowPos(window, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    parameters.BackBufferWidth = width;
    parameters.BackBufferHeight = height;
    const HRESULT result = device->Reset(&parameters);
    std::printf("M2 reset: HRESULT=0x%08lX size=%ldx%ld\n",
                static_cast<unsigned long>(result), width, height);
    if (SUCCEEDED(result) && persistentTexture != nullptr) {
        D3DLOCKED_RECT locked{};
        const HRESULT lockResult =
            persistentTexture->LockRect(0, &locked, nullptr, 0);
        if (FAILED(lockResult)) {
            std::fprintf(
                stderr,
                "Managed texture did not survive reset: HRESULT=0x%08lX\n",
                static_cast<unsigned long>(lockResult));
            return lockResult;
        }
        persistentTexture->UnlockRect(0);
    }
    return result;
}

} // namespace

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001UL;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argumentCount, char** arguments) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const Options options = ParseOptions(argumentCount, arguments);
    const std::uint64_t frameLimit = options.frameLimit;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t className[] = L"FearVrM2D3d9Producer";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor =
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = className;
    if (RegisterClassW(&windowClass) == 0) {
        std::fprintf(stderr, "RegisterClassW failed: %lu\n", GetLastError());
        return 2;
    }

    HWND window = CreateWindowExW(
        0, className, L"F.E.A.R. VR M2 D3D9 Producer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 960,
        540, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "CreateWindowExW failed: %lu\n", GetLastError());
        return 3;
    }

    IDirect3D9Ex* d3dEx = nullptr;
    const HRESULT createD3dResult =
        Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dEx);
    if (FAILED(createD3dResult) || d3dEx == nullptr) {
        std::fprintf(stderr, "Direct3DCreate9Ex failed: 0x%08lX\n",
                     static_cast<unsigned long>(createD3dResult));
        DestroyWindow(window);
        return 4;
    }
    IDirect3D9* d3d = options.classicD3D9
        ? Direct3DCreate9(D3D_SDK_VERSION)
        : static_cast<IDirect3D9*>(d3dEx);
    if (d3d == nullptr) {
        std::fprintf(stderr, "Direct3DCreate9 failed.\n");
        d3dEx->Release();
        DestroyWindow(window);
        return 4;
    }

    UINT selectedAdapter = D3DADAPTER_DEFAULT;
    bool selectedAdapterFound = options.adapterLuid == 0;
    for (UINT adapter = 0; adapter < d3dEx->GetAdapterCount(); ++adapter) {
        LUID luid{};
        D3DADAPTER_IDENTIFIER9 identifier{};
        if (FAILED(d3dEx->GetAdapterLUID(adapter, &luid)) ||
            FAILED(d3dEx->GetAdapterIdentifier(
                adapter, 0, &identifier))) {
            continue;
        }
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(
                 static_cast<std::uint32_t>(luid.HighPart))
             << 32U) |
            luid.LowPart;
        std::printf("M2 D3D9 adapter=%u luid=0x%016llX name=%s\n", adapter,
                    static_cast<unsigned long long>(packed),
                    identifier.Description);
        if (options.adapterLuid != 0 && packed == options.adapterLuid) {
            selectedAdapter = adapter;
            selectedAdapterFound = true;
        }
    }
    if (!selectedAdapterFound) {
        std::fprintf(stderr,
                     "Requested adapter LUID 0x%016llX is unavailable.\n",
                     static_cast<unsigned long long>(
                         options.adapterLuid));
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 6;
    }

    D3DDISPLAYMODE adapterMode{};
    if (options.fullscreen &&
        FAILED(d3d->GetAdapterDisplayMode(
            selectedAdapter, &adapterMode))) {
        std::fprintf(stderr, "GetAdapterDisplayMode failed.\n");
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 12;
    }

    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth =
        options.fullscreen ? adapterMode.Width : 960;
    parameters.BackBufferHeight =
        options.fullscreen ? adapterMode.Height : 540;
    parameters.BackBufferFormat = options.fullscreen
        ? adapterMode.Format
        : D3DFMT_X8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = options.multisample4
        ? D3DMULTISAMPLE_4_SAMPLES
        : D3DMULTISAMPLE_NONE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = options.fullscreen ? FALSE : TRUE;
    parameters.FullScreen_RefreshRateInHz =
        options.fullscreen && !options.fullscreenDefaultRefresh
            ? adapterMode.RefreshRate
            : 0;
    parameters.EnableAutoDepthStencil = FALSE;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    HRESULT result = D3DERR_INVALIDCALL;
    if (options.classicD3D9) {
        result = d3d->CreateDevice(
            selectedAdapter, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING |
                D3DCREATE_MULTITHREADED,
            &parameters, &device);
        if (FAILED(result)) {
            result = d3d->CreateDevice(
                selectedAdapter, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                    D3DCREATE_MULTITHREADED,
                &parameters, &device);
        }
    } else {
        IDirect3DDevice9Ex* deviceEx = nullptr;
        D3DDISPLAYMODEEX fullscreenMode{};
        D3DDISPLAYMODEEX* fullscreenModePointer = nullptr;
        if (options.fullscreen) {
            fullscreenMode.Size = sizeof(fullscreenMode);
            fullscreenMode.Width = adapterMode.Width;
            fullscreenMode.Height = adapterMode.Height;
            fullscreenMode.RefreshRate = adapterMode.RefreshRate;
            fullscreenMode.Format = adapterMode.Format;
            fullscreenMode.ScanLineOrdering =
                D3DSCANLINEORDERING_PROGRESSIVE;
            fullscreenModePointer = &fullscreenMode;
        }
        result = d3dEx->CreateDeviceEx(
            selectedAdapter, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING |
                D3DCREATE_MULTITHREADED,
            &parameters, fullscreenModePointer, &deviceEx);
        if (FAILED(result)) {
            result = d3dEx->CreateDeviceEx(
                selectedAdapter, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                    D3DCREATE_MULTITHREADED,
                &parameters, fullscreenModePointer, &deviceEx);
        }
        device = deviceEx;
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "CreateDevice failed: 0x%08lX\n",
                     static_cast<unsigned long>(result));
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 5;
    }

    IDirect3DDevice9Ex* compatibilityDeviceEx = nullptr;
    const HRESULT queryExResult = device->QueryInterface(
        __uuidof(IDirect3DDevice9Ex),
        reinterpret_cast<void**>(&compatibilityDeviceEx));
    if (options.expectExDevice &&
        (FAILED(queryExResult) || compatibilityDeviceEx == nullptr)) {
        std::fprintf(
            stderr,
            "Expected IDirect3DDevice9Ex compatibility device: "
            "HRESULT=0x%08lX\n",
            static_cast<unsigned long>(queryExResult));
        device->Release();
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 10;
    }
    std::printf(
        "M2 D3D9 device interface: ex=%s\n",
        compatibilityDeviceEx == nullptr ? "no" : "yes");
    if (compatibilityDeviceEx != nullptr) {
        compatibilityDeviceEx->Release();
    }

    IDirect3DTexture9* persistentManagedTexture = nullptr;
    if (options.probeManagedResources &&
        !ProbeManagedResources(device, &persistentManagedTexture)) {
        device->Release();
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 11;
    }

    IndexedMarker indexedMarker;
    if (options.compatibilitySmoke) {
        result = CreateIndexedMarker(device, indexedMarker);
        if (FAILED(result)) {
            std::fprintf(
                stderr,
                "Persistent managed indexed marker creation failed: "
                "HRESULT=0x%08lX\n",
                static_cast<unsigned long>(result));
            if (persistentManagedTexture != nullptr) {
                persistentManagedTexture->Release();
            }
            device->Release();
            d3d->Release();
            d3dEx->Release();
            DestroyWindow(window);
            return 14;
        }
    }

    std::printf(
        "M2 D3D9 producer started: frames=%llu mode=%s msaa=%u\n",
                static_cast<unsigned long long>(frameLimit),
                options.classicD3D9 ? "classic" : "ex",
                static_cast<unsigned>(parameters.MultiSampleType));
    HMODULE proxy = GetModuleHandleW(L"d3d9.dll");
    const auto beginEye = proxy == nullptr
        ? nullptr
        : reinterpret_cast<BeginEyeFunction>(
              GetProcAddress(proxy, "FearVr_BeginEye"));
    const auto captureEye = proxy == nullptr
        ? nullptr
        : reinterpret_cast<CaptureEyeFunction>(
              GetProcAddress(proxy, "FearVr_CaptureEye"));
    const auto endStereoFrame = proxy == nullptr
        ? nullptr
        : reinterpret_cast<EndStereoFrameFunction>(
              GetProcAddress(proxy, "FearVr_EndStereoFrame"));
    if (options.stereo &&
        (beginEye == nullptr || captureEye == nullptr ||
         endStereoFrame == nullptr)) {
        std::fprintf(stderr, "M3 stereo bridge exports are unavailable.\n");
        device->Release();
        if (options.classicD3D9) {
            d3d->Release();
        }
        d3dEx->Release();
        DestroyWindow(window);
        return 7;
    }
    if (options.stereo) {
        std::printf(
            "M3 stereo producer active: left=red right=blue\n");
    }

    IDirect3DSwapChain9* presentationSwapChain = nullptr;
    if (options.swapChainPresent) {
        result = device->GetSwapChain(0, &presentationSwapChain);
        if (FAILED(result) || presentationSwapChain == nullptr) {
            std::fprintf(
                stderr,
                "GetSwapChain failed: HRESULT=0x%08lX\n",
                static_cast<unsigned long>(result));
            device->Release();
            return 13;
        }
    }

    bool resetDone = false;
    bool beforeResetPixelPassed = false;
    bool afterResetPixelPassed = false;
    bool compatibilityFailure = false;
    HRESULT firstFrameFailure = D3D_OK;
    bool running = true;
    for (std::uint64_t frame = 1; running && frame <= frameLimit; ++frame) {
        running = PumpMessages();

        if (!options.fullscreen && frame == frameLimit / 3U) {
            ShowWindow(window, SW_MINIMIZE);
            Sleep(100);
            ShowWindow(window, SW_RESTORE);
            std::printf("M2 minimize/restore completed at frame=%llu\n",
                        static_cast<unsigned long long>(frame));
        }
        if (!options.fullscreen && !resetDone &&
            frame >= frameLimit / 2U) {
            resetDone = SUCCEEDED(ResetDevice(
                device, parameters, window,
                persistentManagedTexture));
        }

        if (options.sceneBoundary) {
            result = device->BeginScene();
        } else {
            result = D3D_OK;
        }
        IDirect3DSurface9* originalTarget = nullptr;
        IDirect3DTexture9* offscreenTexture = nullptr;
        IDirect3DSurface9* offscreenSurface = nullptr;
        const bool offscreenFinalFrame =
            options.offscreenFinal && frame >= 30 && frame < 50;
        if (SUCCEEDED(result) && offscreenFinalFrame) {
            result = device->GetRenderTarget(0, &originalTarget);
            if (SUCCEEDED(result)) {
                result = device->Clear(
                    0, nullptr, D3DCLEAR_TARGET,
                    D3DCOLOR_XRGB(0, 0, 0), 1.0F, 0);
            }
            if (SUCCEEDED(result)) {
                result = device->CreateTexture(
                    parameters.BackBufferWidth,
                    parameters.BackBufferHeight, 1,
                    D3DUSAGE_RENDERTARGET,
                    parameters.BackBufferFormat, D3DPOOL_DEFAULT,
                    &offscreenTexture, nullptr);
            }
            if (SUCCEEDED(result)) {
                result = offscreenTexture->GetSurfaceLevel(
                    0, &offscreenSurface);
            }
            if (SUCCEEDED(result)) {
                result = device->SetRenderTarget(
                    0, offscreenSurface);
            }
            if (SUCCEEDED(result)) {
                result = device->Clear(
                    0, nullptr, D3DCLEAR_TARGET,
                    D3DCOLOR_XRGB(32, 240, 64), 1.0F, 0);
            }
        } else if (SUCCEEDED(result) && options.stereo) {
            result = D3D_OK;
            for (std::uint32_t eye = 0; eye < 2; ++eye) {
                result = device->Clear(
                    0, nullptr, D3DCLEAR_TARGET,
                    StereoEyeColor(frame, eye), 1.0F, 0);
                if (FAILED(result)) {
                    break;
                }
                beginEye(eye);
                captureEye(eye);
            }
            endStereoFrame(frame);
        } else if (SUCCEEDED(result)) {
            result = device->Clear(
                0, nullptr, D3DCLEAR_TARGET,
                FrameColor(frame), 1.0F, 0);
        }
        if (options.sceneBoundary && !options.stereo &&
            SUCCEEDED(result)) {
            result = options.compatibilitySmoke
                ? DrawIndexedMarker(device, indexedMarker)
                : DrawMarker(
                    device, D3DCOLOR_XRGB(255, 255, 255));
        }
        if (options.sceneBoundary && SUCCEEDED(result)) {
            result = device->EndScene();
        }
        if (options.compatibilitySmoke && SUCCEEDED(result) &&
            !beforeResetPixelPassed && !resetDone && frame >= 2) {
            beforeResetPixelPassed =
                VerifyIndexedMarkerPixel(device, "before-reset");
            compatibilityFailure = !beforeResetPixelPassed;
        }
        if (options.compatibilitySmoke && SUCCEEDED(result) &&
            !afterResetPixelPassed && resetDone) {
            afterResetPixelPassed =
                VerifyIndexedMarkerPixel(device, "after-reset");
            compatibilityFailure =
                compatibilityFailure || !afterResetPixelPassed;
        }
        if (SUCCEEDED(result)) {
            if (presentationSwapChain != nullptr) {
                result = presentationSwapChain->Present(
                    nullptr, nullptr, nullptr, nullptr, 0);
            } else if (options.presentEx) {
                IDirect3DDevice9Ex* presentDevice = nullptr;
                result = device->QueryInterface(
                    __uuidof(IDirect3DDevice9Ex),
                    reinterpret_cast<void**>(&presentDevice));
                if (SUCCEEDED(result) && presentDevice != nullptr) {
                    result = presentDevice->PresentEx(
                        nullptr, nullptr, nullptr, nullptr, 0);
                    presentDevice->Release();
                }
            } else {
                result = device->Present(
                    nullptr, nullptr, nullptr, nullptr);
            }
        }
        if (originalTarget != nullptr) {
            const HRESULT restoreResult =
                device->SetRenderTarget(0, originalTarget);
            if (SUCCEEDED(result) && FAILED(restoreResult)) {
                result = restoreResult;
            }
        }
        if (offscreenSurface != nullptr) {
            offscreenSurface->Release();
        }
        if (offscreenTexture != nullptr) {
            offscreenTexture->Release();
        }
        if (originalTarget != nullptr) {
            originalTarget->Release();
        }
        if (result == D3DERR_DEVICELOST) {
            Sleep(10);
            continue;
        }
        if (FAILED(result) && SUCCEEDED(firstFrameFailure)) {
            firstFrameFailure = result;
        }
        if (FAILED(result)) {
            std::fprintf(stderr,
                         "Frame %llu failed: HRESULT=0x%08lX\n",
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long>(result));
            break;
        }
        if (frame == 1 || frame % 300U == 0) {
            if (options.stereo) {
                std::printf(
                    "M3 producer frame=%llu left=0x%08lX right=0x%08lX\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long>(
                        StereoEyeColor(frame, 0)),
                    static_cast<unsigned long>(
                        StereoEyeColor(frame, 1)));
            } else {
                std::printf("M2 producer frame=%llu color=0x%08lX\n",
                            static_cast<unsigned long long>(frame),
                            static_cast<unsigned long>(
                                FrameColor(frame)));
            }
        }
        Sleep(11);
    }

    if (presentationSwapChain != nullptr) {
        presentationSwapChain->Release();
    }
    if (persistentManagedTexture != nullptr) {
        persistentManagedTexture->Release();
    }
    ReleaseIndexedMarker(indexedMarker);
    device->Release();
    if (options.classicD3D9) {
        d3d->Release();
    }
    d3dEx->Release();
    DestroyWindow(window);
    UnregisterClassW(className, instance);
    std::printf("M2 D3D9 producer stopped.\n");
    if (FAILED(firstFrameFailure)) {
        std::fprintf(
            stderr,
            "D3D9 frame processing failed: HRESULT=0x%08lX\n",
            static_cast<unsigned long>(firstFrameFailure));
        return 15;
    }
    if (options.compatibilitySmoke &&
        (!beforeResetPixelPassed || !afterResetPixelPassed ||
         compatibilityFailure)) {
        std::fprintf(
            stderr,
            "D3D9Ex compatibility smoke test: FAIL\n");
        return 14;
    }
    if (options.compatibilitySmoke) {
        std::printf("D3D9Ex compatibility smoke test: PASS\n");
    }
    return 0;
}
