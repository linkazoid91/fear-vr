#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>

#include <cstdio>

int main() {
    IDirect3D9* direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (direct3d == nullptr) {
        std::fprintf(stderr, "Direct3DCreate9 failed\n");
        return 1;
    }
    direct3d->Release();

    HMODULE proxy = GetModuleHandleW(L"d3d9.dll");
    if (proxy == nullptr ||
        GetProcAddress(proxy, "FearVr_InstallIatHook") == nullptr) {
        std::fprintf(stderr, "app-local FearVR d3d9.dll is not loaded\n");
        return 2;
    }

    HMODULE gameClient = LoadLibraryExW(
        L"GameClient.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (gameClient == nullptr) {
        std::fprintf(stderr, "GameClient.dll load failed: %lu\n",
                     GetLastError());
        return 3;
    }

    using GetBuildNumberFunction = unsigned long(__cdecl*)();
    const auto getBuildNumber =
        reinterpret_cast<GetBuildNumberFunction>(
            GetProcAddress(gameClient, "GetBuildNumber"));
    if (getBuildNumber == nullptr) {
        std::fprintf(stderr, "GetBuildNumber export is missing\n");
        FreeLibrary(gameClient);
        return 4;
    }

    // GameOrig.dll is intentionally absent. The call still forces
    // GameClient.dll to initialize its bridge selection.
    (void)getBuildNumber();

    if (GetModuleHandleW(L"fearvr-d3d9.dll") != nullptr) {
        std::fprintf(stderr,
                     "GameClient loaded a second FearVR bridge module\n");
        FreeLibrary(gameClient);
        return 5;
    }

    using AreLateHooksActiveFunction = BOOL(__cdecl*)();
    const auto areLateHooksActive =
        reinterpret_cast<AreLateHooksActiveFunction>(
            GetProcAddress(proxy, "FearVr_AreLateHooksActive"));
    if (areLateHooksActive == nullptr || areLateHooksActive()) {
        std::fprintf(
            stderr,
            "GameClient activated legacy late hooks despite reusing "
            "the app-local proxy\n");
        FreeLibrary(gameClient);
        return 6;
    }
    std::printf("GameClient reused the app-local d3d9.dll bridge\n");
    FreeLibrary(gameClient);
    return 0;
}
