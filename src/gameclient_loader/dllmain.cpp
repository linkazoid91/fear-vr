#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include "stereo_hook.h"

extern "C" int FearVrGameClientCompatData = 0;

namespace {

INIT_ONCE g_originalOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_bridgeOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_original = nullptr;
HMODULE g_bridge = nullptr;

bool IsFearVrBridge(HMODULE module) noexcept {
    return module != nullptr &&
        GetProcAddress(module, "FearVr_InstallIatHook") != nullptr &&
        GetProcAddress(module, "FearVr_ApplyEngineFixes") != nullptr &&
        GetProcAddress(module, "FearVr_BeginEye") != nullptr &&
        GetProcAddress(module, "FearVr_ReportHookStatus") != nullptr;
}

HMODULE FindLoadedAppLocalProxy() noexcept {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    do {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE, GetCurrentProcessId());
    } while (snapshot == INVALID_HANDLE_VALUE &&
             GetLastError() == ERROR_BAD_LENGTH);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    HMODULE result = nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"d3d9.dll") == 0 &&
                IsFearVrBridge(entry.hModule)) {
                result = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool ModuleSiblingPath(const wchar_t* fileName,
                       wchar_t (&path)[MAX_PATH]) noexcept {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleSiblingPath), &self)) {
        return false;
    }
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    wchar_t* separator = wcsrchr(path, L'\\');
    if (separator == nullptr) {
        return false;
    }
    *(separator + 1) = L'\0';
    return wcscat_s(path, fileName) == 0;
}

BOOL CALLBACK LoadBridge(PINIT_ONCE once, PVOID parameter,
                         PVOID* context) {
    (void)once;
    (void)parameter;
    (void)context;

    // An app-local proxy loads before GameClient.dll. Reuse it so device
    // hooks, IPC, and stereo callbacks all share one bridge instance.
    bool reusedAppLocalProxy = false;
    HMODULE appLocalProxy = FindLoadedAppLocalProxy();
    if (IsFearVrBridge(appLocalProxy)) {
        g_bridge = appLocalProxy;
        reusedAppLocalProxy = true;
    }

    wchar_t path[MAX_PATH]{};
    if (g_bridge == nullptr) {
        if (!ModuleSiblingPath(L"fearvr-d3d9.dll", path)) {
            return TRUE;
        }
        g_bridge = LoadLibraryExW(
            path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (g_bridge != nullptr && !reusedAppLocalProxy) {
        using InstallFunction = BOOL(__cdecl*)();
        const auto install = reinterpret_cast<InstallFunction>(
            GetProcAddress(g_bridge, "FearVr_InstallIatHook"));
        if (install != nullptr) {
            install();
        }
    } else if (reusedAppLocalProxy) {
        // The app-local proxy already owns Direct3DCreate9. Installing the
        // legacy hooks would create a second bridge path around that proxy.
        using ApplyEngineFixesFunction = void(__cdecl*)();
        const auto applyEngineFixes =
            reinterpret_cast<ApplyEngineFixesFunction>(
                GetProcAddress(g_bridge, "FearVr_ApplyEngineFixes"));
        if (applyEngineFixes != nullptr) {
            applyEngineFixes();
        }
        using ReportFunction =
            void(__cdecl*)(const char*, const char*, const char*);
        const auto report = reinterpret_cast<ReportFunction>(
            GetProcAddress(g_bridge, "FearVr_ReportHookStatus"));
        if (report != nullptr) {
            report(
                "INFO", "app_local_proxy_reused",
                "App-local d3d9.dll already owns device creation; "
                "legacy IAT and late hooks were skipped.");
        }
    }
    return TRUE;
}

void EnsureBridge() noexcept {
    InitOnceExecuteOnce(&g_bridgeOnce, LoadBridge, nullptr, nullptr);
}

BOOL CALLBACK LoadOriginal(PINIT_ONCE once, PVOID parameter,
                           PVOID* context) {
    (void)once;
    (void)parameter;
    (void)context;

    wchar_t path[MAX_PATH]{};
    if (!ModuleSiblingPath(L"GameOrig.dll", path)) {
        return TRUE;
    }
    g_original = LoadLibraryExW(
        path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return TRUE;
}

HMODULE OriginalModule() noexcept {
    InitOnceExecuteOnce(&g_originalOnce, LoadOriginal, nullptr, nullptr);
    return g_original;
}

} // namespace

extern "C" unsigned long GetBuildNumber() {
    EnsureBridge();
    using Function = unsigned long(__cdecl*)();
    HMODULE original = OriginalModule();
    const auto function = original == nullptr
        ? nullptr
        : reinterpret_cast<Function>(
              GetProcAddress(original, "GetBuildNumber"));
    return function == nullptr ? 0UL : function();
}

extern "C" void SetMasterDatabase(void* masterDatabase) {
    EnsureBridge();
    using Function = void(__cdecl*)(void*);
    HMODULE original = OriginalModule();
    const auto function = original == nullptr
        ? nullptr
        : reinterpret_cast<Function>(
              GetProcAddress(original, "SetMasterDatabase"));
    if (function != nullptr) {
        function(masterDatabase);
    }
    fearvr::InstallStereoHook(masterDatabase, g_bridge);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
