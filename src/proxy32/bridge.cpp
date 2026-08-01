#include "bridge.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include <Shellapi.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <wrl/client.h>

#include "d3d9ex_compat.h"
#include "fearvr-version.h"
#include "ipc_names.h"
#include "protocol_utils.h"
#include "render_scale.h"
#include "stereo_hud_math.h"
#include "system_d3d9.h"

namespace fearvr {
namespace {

using Microsoft::WRL::ComPtr;

thread_local bool g_internalViewportStateChange = false;

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string UtcTimestamp(bool fileSafe) {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << time.wYear;
    if (fileSafe) {
        output << std::setw(2) << time.wMonth << std::setw(2) << time.wDay
               << '-' << std::setw(2) << time.wHour << std::setw(2)
               << time.wMinute << std::setw(2) << time.wSecond << '-'
               << GetCurrentProcessId();
    } else {
        output << '-' << std::setw(2) << time.wMonth << '-' << std::setw(2)
               << time.wDay << 'T' << std::setw(2) << time.wHour << ':'
               << std::setw(2) << time.wMinute << ':' << std::setw(2)
               << time.wSecond << '.' << std::setw(3) << time.wMilliseconds
               << 'Z';
    }
    return output.str();
}

class Logger {
public:
    void Open(const std::filesystem::path& directory) noexcept {
        try {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                return;
            }
            path_ = directory /
                    ("proxy-" + UtcTimestamp(true) + ".log");
            stream_.open(path_, std::ios::out | std::ios::trunc);
        } catch (...) {
        }
    }

    void Write(const char* level, const char* event,
               const std::string& message) noexcept {
        try {
            if (!stream_.is_open()) {
                return;
            }
            stream_ << "{\"time\":\"" << UtcTimestamp(false)
                    << "\",\"level\":\"" << JsonEscape(level)
                    << "\",\"event\":\"" << JsonEscape(event)
                    << "\",\"message\":\"" << JsonEscape(message)
                    << "\"}\n";
            stream_.flush();
        } catch (...) {
        }
    }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
};

struct CommandLineConfig {
    std::uint64_t sessionId{0};
    std::filesystem::path logDirectory;
    bool stereoEnabled{false};
    bool stereoToggleAllowed{false};
    bool translationEnabled{false};
    bool stereoHudEnabled{false};
    // Notausstieg: laesst den GPU-Kompositor weg und mischt das HUD wieder
    // Pixel fuer Pixel auf der CPU. Der GPU-Weg zeichnet in das Geraet des
    // Spiels; bleibt danach etwas schwarz, trennt dieser Schalter die Ursache.
    bool disableGpuHud{false};
    // Diagnostic escape hatch for comparing against beta.7's immediate
    // CPU-readback scheduling (the old end-of-Present spin stays removed).
    bool syncCpuBridge{false};
    // Measures the game's unmodified Present rate while leaving the proxy,
    // OpenXR host, and gameplay hooks loaded. No frame is copied to the host.
    bool disableCapture{false};
    // Diagnostic rollback for the three verified Jupiter EX input patches.
    bool disableHidFpsFix{false};
    // Diagnostic rollback for pacing the game from fresh OpenXR requests.
    bool disableXrFramePacing{false};
    // Linear supersampling applied only while the native stereo world is
    // rendered. Retail's display mode remains unchanged for menus/videos.
    std::uint32_t renderScalePercent{kRenderScaleMinimumPercent};
};

CommandLineConfig ReadConfig() noexcept {
    CommandLineConfig config;
    int argumentCount = 0;
    wchar_t** arguments =
        CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return config;
    }
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], L"-fearvr-session") == 0 &&
            index + 1 < argumentCount) {
            wchar_t* end = nullptr;
            const unsigned long long parsed =
                _wcstoui64(arguments[++index], &end, 0);
            if (end != arguments[index] && *end == L'\0' && parsed != 0) {
                config.sessionId = static_cast<std::uint64_t>(parsed);
            }
        } else if (_wcsicmp(arguments[index], L"-fearvr-logdir") == 0 &&
                   index + 1 < argumentCount) {
            config.logDirectory = arguments[++index];
        } else if (_wcsicmp(
                       arguments[index], L"-fearvr-stereo") == 0) {
            config.stereoEnabled = true;
        } else if (_wcsicmp(
                       arguments[index], L"-fearvr-stereo-toggle") == 0) {
            config.stereoToggleAllowed = true;
        } else if (_wcsicmp(
                       arguments[index], L"-fearvr-translation") == 0) {
            config.translationEnabled = true;
        } else if (_wcsicmp(
                       arguments[index], L"-fearvr-stereo-hud") == 0) {
            config.stereoHudEnabled = true;
        } else if (_wcsicmp(
                       arguments[index], L"-fearvr-no-gpu-hud") == 0) {
            config.disableGpuHud = true;
        } else if (_wcsicmp(
                       arguments[index],
                       L"-fearvr-sync-cpu-bridge") == 0) {
            config.syncCpuBridge = true;
        } else if (_wcsicmp(
                       arguments[index],
                       L"-fearvr-no-capture") == 0) {
            config.disableCapture = true;
        } else if (_wcsicmp(
                       arguments[index],
                       L"-fearvr-no-hid-fps-fix") == 0) {
            config.disableHidFpsFix = true;
        } else if (_wcsicmp(
                       arguments[index],
                       L"-fearvr-no-xr-frame-pacing") == 0) {
            config.disableXrFramePacing = true;
        } else if (_wcsicmp(
                       arguments[index],
                       L"-fearvr-render-scale") == 0 &&
                   index + 1 < argumentCount) {
            wchar_t* end = nullptr;
            const unsigned long parsed =
                std::wcstoul(arguments[++index], &end, 10);
            if (end != arguments[index] && *end == L'\0') {
                config.renderScalePercent =
                    NormalizeRenderScalePercent(
                        static_cast<std::uint32_t>(parsed));
            }
        }
    }
    LocalFree(arguments);
    return config;
}

enum class HidFpsFixResult {
    Applied,
    AlreadyApplied,
    UnsupportedExecutable,
    ByteMismatch,
    ProtectFailed
};

HidFpsFixResult ApplyVerifiedHidFpsFix() noexcept {
    // Steam F.E.A.R. 1.08, identified independently by the release installer
    // and verified here again before touching executable memory.
    constexpr DWORD kFearSteam108Timestamp = 0x44EF6AE6;
    struct Patch {
        std::uintptr_t rva;
        const std::uint8_t* expected;
        std::size_t size;
    };
    static constexpr std::uint8_t kHidControllerInit1[] = {
        0x6A, 0x00, 0x6A, 0x00, 0xFF, 0x15, 0x70, 0xC0, 0x54, 0x00,
        0x50, 0x68, 0xC0, 0x0C, 0x48, 0x00, 0x6A, 0x0D, 0xFF, 0x15,
        0x3C, 0xC4, 0x54, 0x00, 0xA3, 0xBC, 0x5B, 0x57, 0x00};
    static constexpr std::uint8_t kHidControllerInit2[] = {
        0x8B, 0x3F, 0x8B, 0x0F, 0x6A, 0x01, 0x8D, 0x54, 0x24, 0x0C,
        0x52, 0x68, 0xD0, 0x2E, 0x48, 0x00, 0x6A, 0x01, 0x57, 0xFF,
        0x51, 0x10};
    static constexpr std::uint8_t kLegacyJoystickInit[] = {
        0x6A, 0x02, 0x57, 0x8B, 0xCE, 0xE8, 0x60, 0xFE, 0xFF, 0xFF,
        0x8B, 0x44, 0x24, 0x14, 0x83, 0xC7, 0x10, 0x3B, 0xF8, 0x75,
        0xEB, 0x8B, 0x7C, 0x24, 0x10};
    static constexpr Patch kPatches[] = {
        {0x84057, kHidControllerInit1, sizeof(kHidControllerInit1)},
        {0x840DD, kHidControllerInit2, sizeof(kHidControllerInit2)},
        {0x84166, kLegacyJoystickInit, sizeof(kLegacyJoystickInit)}};

    auto* const base = reinterpret_cast<std::uint8_t*>(
        GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return HidFpsFixResult::UnsupportedExecutable;
    }
    const auto* const dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return HidFpsFixResult::UnsupportedExecutable;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != kFearSteam108Timestamp) {
        return HidFpsFixResult::UnsupportedExecutable;
    }

    bool allNops = true;
    for (const Patch& patch : kPatches) {
        const std::uint8_t* const target = base + patch.rva;
        bool patchIsNops = true;
        for (std::size_t index = 0; index < patch.size; ++index) {
            patchIsNops = patchIsNops && target[index] == 0x90;
        }
        if (!patchIsNops &&
            std::memcmp(target, patch.expected, patch.size) != 0) {
            return HidFpsFixResult::ByteMismatch;
        }
        allNops = allNops && patchIsNops;
    }
    if (allNops) {
        return HidFpsFixResult::AlreadyApplied;
    }

    constexpr std::uintptr_t kFirstRva = 0x84057;
    constexpr std::uintptr_t kLastRva =
        0x84166 + sizeof(kLegacyJoystickInit);
    DWORD oldProtection = 0;
    if (!VirtualProtect(
            base + kFirstRva, kLastRva - kFirstRva,
            PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return HidFpsFixResult::ProtectFailed;
    }
    for (const Patch& patch : kPatches) {
        std::memset(base + patch.rva, 0x90, patch.size);
    }
    DWORD ignored = 0;
    VirtualProtect(
        base + kFirstRva, kLastRva - kFirstRva,
        oldProtection, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(), base + kFirstRva,
        kLastRva - kFirstRva);
    return HidFpsFixResult::Applied;
}

volatile LONG* AtomicState(FearVrSlot& slot) noexcept {
    return reinterpret_cast<volatile LONG*>(&slot.state);
}

volatile LONG* AtomicFlags(FearVrSharedHeader& header) noexcept {
    return reinterpret_cast<volatile LONG*>(&header.bridgeFlags);
}

volatile LONG* Atomic32(std::uint32_t& value) noexcept {
    return reinterpret_cast<volatile LONG*>(&value);
}

volatile LONG64* Atomic64(std::uint64_t& value) noexcept {
    return reinterpret_cast<volatile LONG64*>(&value);
}

std::uint64_t ReadAtomic64(std::uint64_t& value) noexcept {
    return static_cast<std::uint64_t>(
        InterlockedCompareExchange64(Atomic64(value), 0, 0));
}

struct SlotResource {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface;
    ComPtr<IDirect3DQuery9> completion;
    HANDLE sharedHandle{nullptr};
};

constexpr std::size_t kCpuCaptureQueueSize = 3;

// Classic D3D9 cannot expose a render target directly to the D3D9Ex bridge
// device. Keep the final eye images on the game GPU until an event query says
// the copies have completed. Only then perform GetRenderTargetData. This moves
// the unavoidable CPU transfer off the critical end-of-frame dependency:
// Present never waits for the frame it has just rendered.
struct CpuCaptureFrame {
    std::array<ComPtr<IDirect3DTexture9>, FEARVR_EYE_COUNT> texture;
    std::array<ComPtr<IDirect3DSurface9>, FEARVR_EYE_COUNT> surface;
    ComPtr<IDirect3DQuery9> completion;
    std::uint64_t frameId{0};
    std::uint32_t renderModeGeneration{0};
    bool active{false};
    bool stereo{false};
    bool flatPanel{false};
};

enum class TransferMode {
    None,
    DirectShared,
    CpuViaD3D9Ex
};

// ============================================================================
// GPU-Kompositor für das Stereo-HUD
//
// Die erste Fassung verglich das Present-Bild und das rechte Weltbild Pixel für
// Pixel auf der CPU. Das kostete pro Bild drei volle Readbacks über den Bus und
// drei Durchläufe über alle Pixel — bei 1080p rund sechs Millionen Iterationen.
// Dieselbe Entscheidung trifft ein Pixelshader auf der GPU, wo die Bilder
// ohnehin schon liegen.
//
// Die Mathematik ist bewusst dieselbe wie in `stereo_hud_math.h`: Schwelle über
// den Farbkanälen, Stauchung um die Bildmitte, und die Auswahl zwischen
// Weltbild und Present. Der Deckungsgrad, der Vollbildeffekte vom HUD trennt,
// entsteht über eine Reduktionskette und wird um ein Bild verzögert gelesen —
// vier Kilobyte statt acht Megabyte, und ohne die Pipeline anzuhalten.
// ============================================================================

// Ab hier wird nicht weiter reduziert; der Rest ist billiger auf der CPU.
constexpr UINT kHudCoverageMaxExtent = 128;
constexpr UINT kHudMaxReduceLevels = 4;

constexpr char kHudMaskShader[] = R"(
sampler2D presented : register(s0);
sampler2D rightWorld : register(s1);
float4 params : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float3 difference = abs(tex2D(presented, uv).rgb -
                            tex2D(rightWorld, uv).rgb);
    float changed = step(
        params.x, max(max(difference.r, difference.g), difference.b));
    return float4(changed, changed, changed, 1.0);
}
)";

constexpr char kHudReduceShader[] = R"(
sampler2D source : register(s0);
float4 texel : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    // Vier bilineare Abgriffe, jeder in der Mitte eines 2x2-Quadranten: das
    // ergibt exakt den Mittelwert der 4x4 Quelltexel dieses Zieltexels.
    float4 total = tex2D(source, uv + float2(-texel.x, -texel.y)) +
                   tex2D(source, uv + float2( texel.x, -texel.y)) +
                   tex2D(source, uv + float2(-texel.x,  texel.y)) +
                   tex2D(source, uv + float2( texel.x,  texel.y));
    return total * 0.25;
}
)";

constexpr char kHudCompositeShader[] = R"(
sampler2D presented : register(s0);
sampler2D rightWorld : register(s1);
sampler2D eyeWorld : register(s2);
float4 params : register(c0);
float4 size : register(c1);
float4 shrink : register(c2);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float4 world = tex2D(eyeWorld, uv);
    float4 flatImage = tex2D(presented, uv);

    // Ganzzahlige Rückrechnung wie StereoHudSourceAxis: Ablage von der Mitte
    // mal 5/4, zur Null hin abgeschnitten.
    float2 outputPixel = floor(uv * size.xy);
    float2 center = floor(size.xy * 0.5);
    float2 scaled = (outputPixel - center) * shrink.x;
    float2 sourcePixel = center + sign(scaled) * floor(abs(scaled));
    float2 sourceUv = (sourcePixel + 0.5) * size.zw;
    float inside = step(0.0, sourcePixel.x) * step(0.0, sourcePixel.y) *
                   step(sourcePixel.x, size.x - 1.0) *
                   step(sourcePixel.y, size.y - 1.0);

    float4 overlayColor = tex2D(presented, sourceUv);
    float3 difference = abs(overlayColor.rgb -
                            tex2D(rightWorld, sourceUv).rgb);
    float changed = step(
        params.x, max(max(difference.r, difference.g), difference.b));

    float overlay = params.y * inside * changed;
    return lerp(lerp(world, overlayColor, overlay), flatImage, params.z);
}
)";

struct HudQuadVertex {
    float x, y, z, rhw;
    float u, v;
};

using D3DCompileFunction = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

class GpuHudCompositor {
public:
    // Legt Shader, Zwischenziele und den Zustandsblock an. Schlägt irgendetwas
    // davon fehl, bleibt der Kompositor einfach aus und der Aufrufer mischt
    // weiter auf der CPU.
    bool Initialize(IDirect3DDevice9* device, UINT width, UINT height,
                    Logger& logger) noexcept {
        Release();
        width_ = width;
        height_ = height;

        D3DCAPS9 caps{};
        if (FAILED(device->GetDeviceCaps(&caps)) ||
            caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
            logger.Write(
                "WARN", "stereo_hud_gpu_unsupported",
                "The game device reports less than pixel shader 2.0; "
                "HUD compositing stays on the CPU.");
            return false;
        }
        if (!LoadCompiler(logger)) {
            return false;
        }
        if (!CompilePixelShader(device, kHudMaskShader, maskShader_,
                                "stereo_hud_mask_shader_failed", logger) ||
            !CompilePixelShader(device, kHudReduceShader, reduceShader_,
                                "stereo_hud_reduce_shader_failed", logger) ||
            !CompilePixelShader(device, kHudCompositeShader,
                                compositeShader_,
                                "stereo_hud_composite_shader_failed",
                                logger)) {
            return false;
        }

        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            if (!CreateRenderTargetTexture(
                    device, width, height, composite_[eye], logger)) {
                return false;
            }
        }
        if (!CreateRenderTargetTexture(
                device, width, height, mask_, logger)) {
            return false;
        }

        UINT levelWidth = width;
        UINT levelHeight = height;
        while (reduceLevelCount_ < kHudMaxReduceLevels &&
               (levelWidth > kHudCoverageMaxExtent ||
                levelHeight > kHudCoverageMaxExtent)) {
            levelWidth = (levelWidth + 3U) / 4U;
            levelHeight = (levelHeight + 3U) / 4U;
            if (!CreateRenderTargetTexture(
                    device, levelWidth, levelHeight,
                    reduce_[reduceLevelCount_], logger)) {
                return false;
            }
            ++reduceLevelCount_;
        }
        coverageWidth_ = levelWidth;
        coverageHeight_ = levelHeight;
        if (reduceLevelCount_ == 0) {
            // Ein sehr kleines Bild braucht keine Reduktion; dann wird die
            // Maske selbst gelesen.
            coverageWidth_ = width;
            coverageHeight_ = height;
        }
        HRESULT result = device->CreateOffscreenPlainSurface(
            coverageWidth_, coverageHeight_, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, coverageReadback_.ReleaseAndGetAddressOf(),
            nullptr);
        if (FAILED(result)) {
            LogHresult(logger, "stereo_hud_coverage_surface_failed", result);
            return false;
        }
        result = device->CreateStateBlock(
            D3DSBT_ALL, stateBlock_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult(logger, "stereo_hud_state_block_failed", result);
            return false;
        }

        ready_ = true;
        coveragePending_ = false;
        std::ostringstream message;
        message << "reduce_levels=" << reduceLevelCount_
                << " coverage=" << coverageWidth_ << 'x' << coverageHeight_;
        logger.Write("INFO", "stereo_hud_gpu_ready", message.str());
        return true;
    }

    void Release() noexcept {
        ready_ = false;
        coveragePending_ = false;
        coverageSource_ = nullptr;
        coverageRatio_ = 1.0;
        reduceLevelCount_ = 0;
        stateBlock_.Reset();
        coverageReadback_.Reset();
        for (auto& level : reduce_) {
            level = {};
        }
        mask_ = {};
        for (auto& target : composite_) {
            target = {};
        }
        compositeShader_.Reset();
        reduceShader_.Reset();
        maskShader_.Reset();
    }

    bool ready() const noexcept { return ready_; }

    IDirect3DSurface9* CompositeSurface(std::uint32_t eye) const noexcept {
        return composite_[eye].surface.Get();
    }

    // Anteil geänderter Pixel aus dem *vorherigen* Bild. Der Wert steuert nur
    // die Betriebsart, nicht die Bildausgabe; ein Bild Verzögerung ist dort
    // belanglos und spart den Synchronisationspunkt.
    double coverageRatio() const noexcept { return coverageRatio_; }

    bool Compose(IDirect3DDevice9* device,
                 IDirect3DTexture9* presented,
                 IDirect3DTexture9* rightWorld,
                 IDirect3DTexture9* const eyeWorld[FEARVR_EYE_COUNT],
                 bool compositeEnabled, bool flatPanel,
                 Logger& logger) noexcept {
        if (!ready_ || stateBlock_->Capture() != D3D_OK) {
            return false;
        }
        ComPtr<IDirect3DSurface9> previousTarget;
        ComPtr<IDirect3DSurface9> previousDepth;
        device->GetRenderTarget(0, previousTarget.ReleaseAndGetAddressOf());
        // Ohne Tiefenpuffer zeichnen: Ein fremdes Ziel anderer Größe würde
        // sonst den Zeichenaufruf ablehnen.
        device->GetDepthStencilSurface(
            previousDepth.ReleaseAndGetAddressOf());

        // Der Present-Hook läuft nach EndScene; ein Zeichenaufruf braucht aber
        // eine offene Szene. Schlägt BeginScene fehl, sind wir bereits in
        // einer — dann darf sie hier auch nicht geschlossen werden.
        const bool beganScene = SUCCEEDED(device->BeginScene());
        bool succeeded = ApplyCommonState(device);
        if (succeeded) {
            ReadPreviousCoverage(logger);
            succeeded = RenderMaskAndReduce(device, presented, rightWorld);
        }
        for (std::uint32_t eye = 0; succeeded && eye < FEARVR_EYE_COUNT;
             ++eye) {
            succeeded = RenderComposite(
                device, presented, rightWorld, eyeWorld[eye],
                composite_[eye], compositeEnabled, flatPanel);
        }
        if (beganScene) {
            device->EndScene();
        }
        if (succeeded && coverageSource_ != nullptr) {
            // Der Kopiervorgang läuft ab hier neben dem Spiel her; gelesen
            // wird er erst im nächsten Bild.
            coveragePending_ = SUCCEEDED(device->GetRenderTargetData(
                coverageSource_, coverageReadback_.Get()));
            coverageSource_ = nullptr;
        }

        device->SetDepthStencilSurface(previousDepth.Get());
        if (previousTarget) {
            device->SetRenderTarget(0, previousTarget.Get());
        }
        stateBlock_->Apply();
        if (!succeeded) {
            logger.Write(
                "WARN", "stereo_hud_gpu_compose_failed",
                "A GPU HUD pass failed; the CPU compositor takes over.");
            ready_ = false;
        }
        return succeeded;
    }

private:
    struct RenderTargetTexture {
        ComPtr<IDirect3DTexture9> texture;
        ComPtr<IDirect3DSurface9> surface;
        UINT width{0};
        UINT height{0};
    };

    static void LogHresult(Logger& logger, const char* event,
                           HRESULT result) noexcept {
        std::ostringstream message;
        message << "HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        logger.Write("ERROR", event, message.str());
    }

    bool LoadCompiler(Logger& logger) noexcept {
        if (compile_ != nullptr) {
            return true;
        }
        // Bewusst dynamisch: Fehlt der Compiler, soll die Bridge weiterlaufen
        // und nicht schon beim Laden der DLL scheitern.
        const HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (module == nullptr) {
            logger.Write(
                "WARN", "stereo_hud_compiler_missing",
                "d3dcompiler_47.dll is unavailable; HUD compositing "
                "stays on the CPU.");
            return false;
        }
        compile_ = reinterpret_cast<D3DCompileFunction>(
            reinterpret_cast<void*>(
                GetProcAddress(module, "D3DCompile")));
        if (compile_ == nullptr) {
            logger.Write(
                "WARN", "stereo_hud_compiler_missing",
                "d3dcompiler_47.dll exports no D3DCompile.");
            return false;
        }
        return true;
    }

    bool CompilePixelShader(IDirect3DDevice9* device, const char* source,
                            ComPtr<IDirect3DPixelShader9>& shader,
                            const char* failureEvent,
                            Logger& logger) noexcept {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        HRESULT result = compile_(
            source, std::strlen(source), "fearvr_stereo_hud", nullptr,
            nullptr, "main", "ps_2_0", 0, 0,
            code.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(result) || !code) {
            if (errors) {
                logger.Write(
                    "ERROR", failureEvent,
                    std::string(
                        static_cast<const char*>(errors->GetBufferPointer()),
                        errors->GetBufferSize()));
            } else {
                LogHresult(logger, failureEvent, result);
            }
            return false;
        }
        result = device->CreatePixelShader(
            static_cast<const DWORD*>(code->GetBufferPointer()),
            shader.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult(logger, failureEvent, result);
            return false;
        }
        return true;
    }

    static bool CreateRenderTargetTexture(IDirect3DDevice9* device,
                                          UINT width, UINT height,
                                          RenderTargetTexture& target,
                                          Logger& logger) noexcept {
        target = {};
        HRESULT result = device->CreateTexture(
            width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, target.texture.ReleaseAndGetAddressOf(),
            nullptr);
        if (FAILED(result)) {
            LogHresult(logger, "stereo_hud_target_create_failed", result);
            return false;
        }
        result = target.texture->GetSurfaceLevel(
            0, target.surface.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult(logger, "stereo_hud_target_surface_failed", result);
            return false;
        }
        target.width = width;
        target.height = height;
        return true;
    }

    bool ApplyCommonState(IDirect3DDevice9* device) noexcept {
        device->SetVertexShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
                               D3DCOLORWRITEENABLE_RED |
                                   D3DCOLORWRITEENABLE_GREEN |
                                   D3DCOLORWRITEENABLE_BLUE |
                                   D3DCOLORWRITEENABLE_ALPHA);
        for (DWORD sampler = 0; sampler < 3; ++sampler) {
            device->SetSamplerState(
                sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                sampler, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetSamplerState(
                sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
        return true;
    }

    static void SetPointFilter(IDirect3DDevice9* device,
                               DWORD sampler) noexcept {
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }

    static void SetLinearFilter(IDirect3DDevice9* device,
                                DWORD sampler) noexcept {
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    }

    static bool DrawFullscreenQuad(IDirect3DDevice9* device, UINT width,
                                   UINT height) noexcept {
        // Die halbe Texelverschiebung ist die D3D9-Regel für 1:1-Abbildung
        // zwischen Pixelmitte und Texelmitte.
        const float right = static_cast<float>(width) - 0.5F;
        const float bottom = static_cast<float>(height) - 0.5F;
        const HudQuadVertex vertices[4] = {
            {-0.5F, -0.5F, 0.0F, 1.0F, 0.0F, 0.0F},
            {right, -0.5F, 0.0F, 1.0F, 1.0F, 0.0F},
            {right, bottom, 0.0F, 1.0F, 1.0F, 1.0F},
            {-0.5F, bottom, 0.0F, 1.0F, 0.0F, 1.0F}};
        return SUCCEEDED(device->DrawPrimitiveUP(
            D3DPT_TRIANGLEFAN, 2, vertices, sizeof(HudQuadVertex)));
    }

    bool RenderMaskAndReduce(IDirect3DDevice9* device,
                             IDirect3DTexture9* presented,
                             IDirect3DTexture9* rightWorld) noexcept {
        const float threshold[4] = {kHudPixelThreshold, 0.0F, 0.0F, 0.0F};
        if (FAILED(device->SetRenderTarget(0, mask_.surface.Get())) ||
            FAILED(device->SetDepthStencilSurface(nullptr)) ||
            FAILED(device->SetPixelShader(maskShader_.Get())) ||
            FAILED(device->SetPixelShaderConstantF(0, threshold, 1)) ||
            FAILED(device->SetTexture(0, presented)) ||
            FAILED(device->SetTexture(1, rightWorld))) {
            return false;
        }
        SetPointFilter(device, 0);
        SetPointFilter(device, 1);
        if (!DrawFullscreenQuad(device, mask_.width, mask_.height)) {
            return false;
        }

        const RenderTargetTexture* source = &mask_;
        if (FAILED(device->SetPixelShader(reduceShader_.Get())) ||
            FAILED(device->SetTexture(1, nullptr))) {
            return false;
        }
        SetLinearFilter(device, 0);
        for (std::uint32_t level = 0; level < reduceLevelCount_; ++level) {
            const RenderTargetTexture& target = reduce_[level];
            const float texel[4] = {
                1.0F / static_cast<float>(source->width),
                1.0F / static_cast<float>(source->height), 0.0F, 0.0F};
            if (FAILED(device->SetRenderTarget(0, target.surface.Get())) ||
                FAILED(device->SetTexture(0, source->texture.Get())) ||
                FAILED(device->SetPixelShaderConstantF(0, texel, 1)) ||
                !DrawFullscreenQuad(device, target.width, target.height)) {
                return false;
            }
            source = &target;
        }
        // Gelesen wird erst nach EndScene: GetRenderTargetData gehört nicht
        // zwischen BeginScene und EndScene.
        coverageSource_ = source->surface.Get();
        return true;
    }

    bool RenderComposite(IDirect3DDevice9* device,
                         IDirect3DTexture9* presented,
                         IDirect3DTexture9* rightWorld,
                         IDirect3DTexture9* eyeWorld,
                         const RenderTargetTexture& target,
                         bool compositeEnabled, bool flatPanel) noexcept {
        const float params[4] = {
            kHudPixelThreshold, compositeEnabled ? 1.0F : 0.0F,
            flatPanel ? 1.0F : 0.0F, 0.0F};
        const float size[4] = {
            static_cast<float>(width_), static_cast<float>(height_),
            1.0F / static_cast<float>(width_),
            1.0F / static_cast<float>(height_)};
        const float shrink[4] = {
            static_cast<float>(kStereoHudShrinkNumerator) /
                static_cast<float>(kStereoHudShrinkDenominator),
            0.0F, 0.0F, 0.0F};
        if (FAILED(device->SetRenderTarget(0, target.surface.Get())) ||
            FAILED(device->SetPixelShader(compositeShader_.Get())) ||
            FAILED(device->SetPixelShaderConstantF(0, params, 1)) ||
            FAILED(device->SetPixelShaderConstantF(1, size, 1)) ||
            FAILED(device->SetPixelShaderConstantF(2, shrink, 1)) ||
            FAILED(device->SetTexture(0, presented)) ||
            FAILED(device->SetTexture(1, rightWorld)) ||
            FAILED(device->SetTexture(2, eyeWorld))) {
            return false;
        }
        SetPointFilter(device, 0);
        SetPointFilter(device, 1);
        SetPointFilter(device, 2);
        return DrawFullscreenQuad(device, target.width, target.height);
    }

    void ReadPreviousCoverage(Logger& logger) noexcept {
        if (!coveragePending_) {
            return;
        }
        coveragePending_ = false;
        D3DLOCKED_RECT locked{};
        const HRESULT result = coverageReadback_->LockRect(
            &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(result)) {
            LogHresult(logger, "stereo_hud_coverage_lock_failed", result);
            return;
        }
        std::uint64_t total = 0;
        for (UINT row = 0; row < coverageHeight_; ++row) {
            const auto* pixels = reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(locked.pBits) +
                static_cast<std::size_t>(row) *
                    static_cast<std::size_t>(locked.Pitch));
            for (UINT column = 0; column < coverageWidth_; ++column) {
                total += pixels[column] & 0xffu;
            }
        }
        coverageReadback_->UnlockRect();
        const std::uint64_t maximum =
            static_cast<std::uint64_t>(coverageWidth_) * coverageHeight_ *
            255ull;
        coverageRatio_ = maximum == 0
            ? 0.0
            : static_cast<double>(total) / static_cast<double>(maximum);
    }

    // 2 von 255 Stufen, wie IsPostWorldPixel: der Vergleich dort ist echt
    // größer, deshalb liegt die Schwelle hier zwischen 2 und 3.
    static constexpr float kHudPixelThreshold = 2.5F / 255.0F;

    D3DCompileFunction compile_{nullptr};
    ComPtr<IDirect3DPixelShader9> maskShader_;
    ComPtr<IDirect3DPixelShader9> reduceShader_;
    ComPtr<IDirect3DPixelShader9> compositeShader_;
    ComPtr<IDirect3DStateBlock9> stateBlock_;
    ComPtr<IDirect3DSurface9> coverageReadback_;
    std::array<RenderTargetTexture, FEARVR_EYE_COUNT> composite_{};
    RenderTargetTexture mask_{};
    std::array<RenderTargetTexture, kHudMaxReduceLevels> reduce_{};
    IDirect3DSurface9* coverageSource_{nullptr};
    std::uint32_t reduceLevelCount_{0};
    UINT width_{0};
    UINT height_{0};
    UINT coverageWidth_{0};
    UINT coverageHeight_{0};
    double coverageRatio_{1.0};
    bool coveragePending_{false};
    bool ready_{false};
};

class Bridge {
public:
    Bridge() : config_(ReadConfig()) {
        if (config_.sessionId != 0) {
            if (config_.logDirectory.empty()) {
                wchar_t temporary[MAX_PATH]{};
                if (GetTempPathW(MAX_PATH, temporary) != 0) {
                    config_.logDirectory =
                        std::filesystem::path(temporary) / "FearVr";
                }
            }
            logger_.Open(config_.logDirectory);
            std::ostringstream message;
            message << "version=" << FEARVR_VERSION_STRING
                    << " git=" << FEARVR_GIT_HASH
                    << " pid=" << GetCurrentProcessId()
                    << " session=0x" << std::hex << std::uppercase
                    << config_.sessionId;
            logger_.Write("INFO", "proxy_start", message.str());
            if (config_.stereoToggleAllowed) {
                logger_.Write(
                    "INFO", "stereo_toggle_ready",
                    "Stereo starts disabled; press F8 in the 3D world. "
                    "Press F9 to recenter head tracking.");
            }
            if (config_.disableXrFramePacing) {
                logger_.Write(
                    "WARN", "xr_frame_pacing_disabled",
                    "FEAR may render duplicate OpenXR requests for "
                    "diagnostic comparison.");
            }
            logger_.Write(
                "INFO", "render_scale_config",
                "requested_percent=" +
                    std::to_string(config_.renderScalePercent) +
                    (config_.renderScalePercent >
                             kRenderScaleMinimumPercent
                         ? " mode=stereo_offscreen"
                         : " mode=retail_backbuffer"));
        }
    }

    ~Bridge() {
        ReleaseResources();
        if (shared_ != nullptr) {
            UnmapViewOfFile(shared_);
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
        if (frameReadyEvent_ != nullptr) {
            CloseHandle(frameReadyEvent_);
        }
        if (slotConsumedEvent_ != nullptr) {
            CloseHandle(slotConsumedEvent_);
        }
        if (renderRequestEvent_ != nullptr) {
            CloseHandle(renderRequestEvent_);
        }
    }

    void LogHookStatus(const char* level, const char* event,
                       const std::string& message) noexcept {
        logger_.Write(level, event, message);
    }

    bool ScaleStereoViewport(
        IDirect3DDevice9* device,
        const D3DVIEWPORT9* requested,
        D3DVIEWPORT9& scaled) noexcept {
        if (device == nullptr || requested == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsSupersampledEyeTargetBound(device) ||
            sourceWidth_ == 0 || sourceHeight_ == 0 ||
            width_ == sourceWidth_ || height_ == sourceHeight_) {
            return false;
        }
        const std::uint64_t sourceRight =
            static_cast<std::uint64_t>(requested->X) +
            requested->Width;
        const std::uint64_t sourceBottom =
            static_cast<std::uint64_t>(requested->Y) +
            requested->Height;
        if (requested->Width == 0 || requested->Height == 0 ||
            sourceRight > sourceWidth_ ||
            sourceBottom > sourceHeight_) {
            return false;
        }

        const UINT left = static_cast<UINT>(MulDiv(
            static_cast<int>(requested->X),
            static_cast<int>(width_),
            static_cast<int>(sourceWidth_)));
        const UINT top = static_cast<UINT>(MulDiv(
            static_cast<int>(requested->Y),
            static_cast<int>(height_),
            static_cast<int>(sourceHeight_)));
        const UINT right = static_cast<UINT>(MulDiv(
            static_cast<int>(sourceRight),
            static_cast<int>(width_),
            static_cast<int>(sourceWidth_)));
        const UINT bottom = static_cast<UINT>(MulDiv(
            static_cast<int>(sourceBottom),
            static_cast<int>(height_),
            static_cast<int>(sourceHeight_)));
        scaled = *requested;
        scaled.X = left;
        scaled.Y = top;
        scaled.Width = (std::max)(1U, right - left);
        scaled.Height = (std::max)(1U, bottom - top);
        if (!stereoViewportScaleLogged_) {
            std::ostringstream message;
            message << "requested=" << requested->X << ','
                    << requested->Y << ' ' << requested->Width
                    << 'x' << requested->Height << " scaled="
                    << scaled.X << ',' << scaled.Y << ' '
                    << scaled.Width << 'x' << scaled.Height;
            logger_.Write(
                "INFO", "render_scale_viewport_adjusted",
                message.str());
            stereoViewportScaleLogged_ = true;
        }
        return true;
    }

    bool ScaleStereoScissor(
        IDirect3DDevice9* device, const RECT* requested,
        RECT& scaled) noexcept {
        if (device == nullptr || requested == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsSupersampledEyeTargetBound(device) ||
            sourceWidth_ == 0 || sourceHeight_ == 0 ||
            width_ == sourceWidth_ || height_ == sourceHeight_ ||
            requested->left < 0 || requested->top < 0 ||
            requested->right <= requested->left ||
            requested->bottom <= requested->top ||
            static_cast<UINT>(requested->right) > sourceWidth_ ||
            static_cast<UINT>(requested->bottom) > sourceHeight_) {
            return false;
        }
        scaled.left = MulDiv(
            requested->left, static_cast<int>(width_),
            static_cast<int>(sourceWidth_));
        scaled.top = MulDiv(
            requested->top, static_cast<int>(height_),
            static_cast<int>(sourceHeight_));
        scaled.right = MulDiv(
            requested->right, static_cast<int>(width_),
            static_cast<int>(sourceWidth_));
        scaled.bottom = MulDiv(
            requested->bottom, static_cast<int>(height_),
            static_cast<int>(sourceHeight_));
        if (!stereoScissorScaleLogged_) {
            std::ostringstream message;
            message << "requested=" << requested->left << ','
                    << requested->top << '-' << requested->right
                    << ',' << requested->bottom << " scaled="
                    << scaled.left << ',' << scaled.top << '-'
                    << scaled.right << ',' << scaled.bottom;
            logger_.Write(
                "INFO", "render_scale_scissor_adjusted",
                message.str());
            stereoScissorScaleLogged_ = true;
        }
        return true;
    }

    void ApplyEngineFixes() noexcept {
        if (config_.disableHidFpsFix) {
            logger_.Write(
                "WARN", "hid_fps_fix_disabled",
                "Jupiter EX HID initialization remains unmodified.");
            return;
        }
        const HidFpsFixResult result = ApplyVerifiedHidFpsFix();
        switch (result) {
        case HidFpsFixResult::Applied:
            logger_.Write(
                "INFO", "hid_fps_fix_applied",
                "Verified Steam 1.08 ranges 0x84057, 0x840DD and "
                "0x84166 disabled redundant HID/joystick initialization.");
            break;
        case HidFpsFixResult::AlreadyApplied:
            logger_.Write(
                "INFO", "hid_fps_fix_already_applied",
                "All three verified Jupiter EX input ranges were already "
                "disabled.");
            break;
        case HidFpsFixResult::UnsupportedExecutable:
            logger_.Write(
                "WARN", "hid_fps_fix_unsupported_executable",
                "Executable timestamp is not Steam F.E.A.R. 1.08; no bytes "
                "were changed.");
            break;
        case HidFpsFixResult::ByteMismatch:
            logger_.Write(
                "ERROR", "hid_fps_fix_byte_mismatch",
                "At least one Jupiter EX input range did not match; no "
                "bytes were changed.");
            break;
        case HidFpsFixResult::ProtectFailed:
            logger_.Write(
                "ERROR", "hid_fps_fix_protect_failed",
                "VirtualProtect rejected the verified input ranges; no "
                "bytes were changed.");
            break;
        }
    }

    void CapturePresent(
        IDirect3DDevice9* device,
        IDirect3DSurface9* presentedBackBuffer = nullptr) noexcept {
        if (device == nullptr || config_.sessionId == 0) {
            return;
        }
        PollStereoToggle();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc()) {
            return;
        }

        InterlockedIncrement64(Atomic64(shared_->gameHeartbeat));
        if (config_.disableCapture) {
            const auto now = std::chrono::steady_clock::now();
            if (bypassPresentCount_ == 0) {
                bypassPresentWindowStart_ = now;
            }
            ++bypassPresentCount_;
            if (bypassPresentCount_ % 300 == 0) {
                const auto elapsedMicroseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - bypassPresentWindowStart_).count();
                const double fps = elapsedMicroseconds > 0
                    ? 300'000'000.0 /
                          static_cast<double>(elapsedMicroseconds)
                    : 0.0;
                std::ostringstream message;
                message.setf(std::ios::fixed);
                message.precision(1);
                message << "presents=300 fps=" << fps
                        << " total=" << bypassPresentCount_;
                logger_.Write(
                    "INFO", "capture_bypass_present_rate",
                    message.str());
                bypassPresentWindowStart_ = now;
            }
            return;
        }
        PollPending();
        UpdateHostConnection();
        EnsureDeviceMetadata(device);
        UpdateAdapterMatch();

        if (!hostConnected_ ||
            (shared_->bridgeFlags & FEARVR_BF_ADAPTER_MATCH) == 0) {
            return;
        }

        ComPtr<IDirect3DSurface9> backBuffer;
        HRESULT result = D3D_OK;
        if (presentedBackBuffer != nullptr) {
            presentedBackBuffer->AddRef();
            backBuffer.Attach(presentedBackBuffer);
        } else {
            result = device->GetBackBuffer(
                0, 0, D3DBACKBUFFER_TYPE_MONO,
                backBuffer.ReleaseAndGetAddressOf());
        }
        if (FAILED(result)) {
            if (result == D3DERR_DEVICELOST) {
                InterlockedOr(AtomicFlags(*shared_), FEARVR_BF_DEVICE_LOST);
            }
            LogHresult("get_backbuffer_failed", result);
            return;
        }

        D3DSURFACE_DESC description{};
        result = backBuffer->GetDesc(&description);
        if (FAILED(result)) {
            LogHresult("get_backbuffer_desc_failed", result);
            return;
        }
        if (!EnsureResources(device, description.Width, description.Height)) {
            return;
        }

        const bool stereo =
            stereoFrameReady_ &&
            stereoEyeCaptured_[FEARVR_EYE_LEFT] &&
            stereoEyeCaptured_[FEARVR_EYE_RIGHT];
        if (stereo && stereoFrameId_ == lastStagedStereoFrameId_) {
            ++stereoDuplicateCaptureDrops_;
            ClearStereoFrame();
            return;
        }
        const bool asyncCpuCapture =
            transferMode_ == TransferMode::CpuViaD3D9Ex &&
            !config_.syncCpuBridge &&
            (!stereo || !config_.stereoHudEnabled ||
             hudCompositor_.ready());
        if (asyncCpuCapture) {
            // Complete an older GPU staging copy before queuing this frame.
            // Neither operation waits for the frame currently being
            // presented. If the staging query is not ready, the game keeps
            // running and the headset reuses its last complete image.
            ProcessCpuCaptureQueue(device);
            const std::uint64_t queuedStereoFrameId = stereoFrameId_;
            if (QueueCpuCapture(device, backBuffer.Get(), stereo) &&
                stereo) {
                lastStagedStereoFrameId_ = queuedStereoFrameId;
            }
            return;
        }
        std::uint32_t slotIndex = 0;
        if (!ClaimWritablePair(slotIndex)) {
            ++droppedFrames_;
            if (droppedFrames_ == 1 || droppedFrames_ % 30000 == 0) {
                logger_.Write(
                    "WARN", "ring_full",
                    "dropped=" + std::to_string(droppedFrames_));
            }
            return;
        }

        const std::uint64_t frameId =
            stereo ? stereoFrameId_ : ++frameId_;
        if (stereo && frameId > frameId_) {
            frameId_ = frameId;
        }
        const std::uint64_t generation = ++generation_;
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            FearVrSlot& slot = shared_->slot[eye][slotIndex];
            slot.frameId = frameId;
            slot.generation = generation;
        }

        bool copied = false;
        stereoHudFlatFrame_ = false;
        if (stereo) {
            copied = transferMode_ == TransferMode::CpuViaD3D9Ex
                ? CopyStereoFrameViaCpu(
                      device, backBuffer.Get(), slotIndex)
                : CopyStereoFrameDirect(device, slotIndex);
        } else {
            copied = transferMode_ == TransferMode::CpuViaD3D9Ex
                ? CopyFrameViaCpu(device, backBuffer.Get(), slotIndex)
                : CopyFrameDirect(device, backBuffer.Get(), slotIndex);
        }

        if (!copied) {
            ReleaseClaimedPair(slotIndex);
            if (stereo) {
                ClearStereoFrame();
            }
            return;
        }
        if (stereo) {
            lastStagedStereoFrameId_ = frameId;
            if (stereoHudFlatFrame_) {
                InterlockedAnd(
                    AtomicFlags(*shared_),
                    static_cast<LONG>(~FEARVR_BF_STEREO_ACTIVE));
            } else {
                InterlockedOr(
                    AtomicFlags(*shared_), FEARVR_BF_STEREO_ACTIVE);
            }
            ++stereoFrames_;
            if (stereoFrames_ == 1 || stereoFrames_ % 300 == 0) {
                logger_.Write(
                    "INFO", "stereo_frame_staged",
                    "request_frame=" + std::to_string(frameId) +
                        " stereo_frames=" +
                        std::to_string(stereoFrames_));
            }
            ClearStereoFrame();
        } else {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(~FEARVR_BF_STEREO_ACTIVE));
        }

        PendingFrame& pending = pending_[slotIndex];
        pending.active = true;
        pending.slotIndex = slotIndex;
        pending.frameId = frameId;
        pending.generation = generation;
        // Completion is polled at the beginning of a later Present. Waiting
        // here couples the game's frame rate to the bridge GPU and was the
        // source of pause/resume wait cascades.
    }

    void BeforeReset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        logger_.Write("INFO", "device_reset_begin",
                      "D3DPOOL_DEFAULT bridge resources released.");
        ReleaseResources();
        if (shared_ != nullptr) {
            InterlockedOr(AtomicFlags(*shared_), FEARVR_BF_DEVICE_LOST);
        }
        device_ = nullptr;
        deviceMetadataReady_ = false;
    }

    void AfterReset(HRESULT result) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_ == nullptr) {
            return;
        }
        if (SUCCEEDED(result)) {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(~FEARVR_BF_DEVICE_LOST));
            logger_.Write("INFO", "device_reset_complete",
                          "Reset successful; resources will be recreated.");
        } else {
            LogHresult("device_reset_failed", result);
        }
    }

    BOOL IsConnected() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc()) {
            return FALSE;
        }
        UpdateHostConnection();
        return hostConnected_ ? TRUE : FALSE;
    }

    BOOL StereoEnabled() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.stereoEnabled ? TRUE : FALSE;
    }

    void SetStereoEnabled(BOOL enabled) noexcept {
        StereoToggleCallback callback = nullptr;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool requested = enabled != FALSE;
            if (config_.stereoEnabled != requested) {
                config_.stereoEnabled = requested;
                ClearStereoFrame();
                if (shared_ != nullptr && !requested) {
                    InterlockedAnd(
                        AtomicFlags(*shared_),
                        static_cast<LONG>(
                            ~FEARVR_BF_STEREO_ACTIVE));
                }
                callback = stereoToggleCallback_;
                changed = true;
                logger_.Write(
                    "INFO", "stereo_set",
                    requested
                        ? "Native stereo enabled automatically after loading."
                        : "Native stereo disabled programmatically.");
            }
        }
        if (changed && callback != nullptr) {
            callback(enabled != FALSE ? TRUE : FALSE);
        }
    }

    void SetFovScalePercent(std::uint32_t percent) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t requested =
            NormalizeFovScalePercent(percent);
        if (fovScalePercent_ == requested) {
            return;
        }
        fovScalePercent_ = requested;
        if (shared_ != nullptr) {
            InterlockedExchange(
                Atomic32(shared_->fovScalePercent),
                static_cast<LONG>(fovScalePercent_));
        }
        logger_.Write(
            "INFO", "fov_scale_set",
            "Stereo FOV scale set to " +
                std::to_string(fovScalePercent_) + "%.");
    }

    BOOL TranslationEnabled() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.translationEnabled ? TRUE : FALSE;
    }

    void SetTranslationEnabled(BOOL enabled) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.translationEnabled = enabled != FALSE;
        logger_.Write(
            "INFO", "translation_set",
            config_.translationEnabled
                ? "Bounded HMD translation enabled from the VR menu."
                : "HMD translation disabled from the VR menu.");
    }

    BOOL StereoHudEnabled() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.stereoHudEnabled ? TRUE : FALSE;
    }

    void SetStereoHudEnabled(BOOL enabled) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.stereoHudEnabled = enabled != FALSE;
        ClearStereoFrame();
        logger_.Write(
            "INFO", "stereo_hud_set",
            config_.stereoHudEnabled
                ? "Stereo HUD enabled from the VR menu."
                : "Stereo HUD disabled from the VR menu.");
    }

    BOOL ComfortModeEnabled() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return comfortModeEnabled_ ? TRUE : FALSE;
    }

    void SetComfortModeEnabled(BOOL enabled) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool requested = enabled != FALSE;
        if (comfortModeEnabled_ == requested) {
            return;
        }
        comfortModeEnabled_ = requested;
        ClearStereoFrame();
        if (shared_ != nullptr) {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(~FEARVR_BF_STEREO_ACTIVE));
        }
        if (!comfortModeEnabled_) {
            IncrementRecenterGeneration();
        }
        logger_.Write(
            "INFO", "comfort_mode_set",
            comfortModeEnabled_
                ? "World-locked comfort panel enabled from the VR menu."
                : "Comfort panel disabled from the VR menu; stereo resumes.");
    }

    void SetMenuActive(BOOL active) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool requested = active != FALSE;
        if (menuActive_ == requested) {
            return;
        }
        menuActive_ = requested;
        ++renderModeGeneration_;
        if (renderModeGeneration_ == 0) {
            ++renderModeGeneration_;
        }
        ClearStereoFrame();
        if (shared_ != nullptr) {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(~FEARVR_BF_STEREO_ACTIVE));
        }
        logger_.Write(
            "INFO", "menu_render_mode",
            menuActive_
                ? "Pause/menu detected; native stereo disabled immediately."
                : "Gameplay detected; native stereo may resume on the next frame.");
    }

    void RequestRecenter() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        IncrementPanelRecenterGeneration();
        logger_.Write(
            "INFO", "panel_recenter_requested",
            "The in-game VR menu requested a new 2D panel anchor.");
    }

    BOOL StereoAvailable() const noexcept {
        return config_.stereoEnabled ||
               config_.stereoToggleAllowed ? TRUE : FALSE;
    }

    BOOL FlatPanelActive() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stereoHudFlatFrame_ ? TRUE : FALSE;
    }

    void RegisterStereoToggle(
        StereoToggleCallback callback) noexcept {
        BOOL enableNow = FALSE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stereoToggleCallback_ = callback;
            enableNow = config_.stereoEnabled ? TRUE : FALSE;
        }
        if (callback != nullptr && enableNow) {
            callback(TRUE);
        }
    }

    BOOL ReadRenderRequest(FearVrRenderRequest* output) noexcept {
        if (output == nullptr) {
            return FALSE;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc()) {
            return FALSE;
        }
        return ReadRenderRequestLocked(output);
    }

    BOOL WaitForNewRenderRequest(
        std::uint64_t previousFrameId,
        std::uint32_t timeoutMilliseconds,
        FearVrRenderRequest* output) noexcept {
        if (output == nullptr || previousFrameId == 0 ||
            config_.disableXrFramePacing) {
            return FALSE;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc() || renderRequestEvent_ == nullptr) {
            return FALSE;
        }

        ServiceCaptureDuringPacingWaitLocked();
        FearVrRenderRequest snapshot{};
        if (ReadRenderRequestLocked(&snapshot) &&
            snapshot.frameId != previousFrameId) {
            *output = snapshot;
            return TRUE;
        }

        constexpr DWORD kMaximumPacingWaitMilliseconds = 20;
        const DWORD boundedTimeout = (std::min)(
            static_cast<DWORD>(timeoutMilliseconds),
            kMaximumPacingWaitMilliseconds);
        if (boundedTimeout == 0) {
            return FALSE;
        }

        if (!xrFramePacingLogged_) {
            xrFramePacingLogged_ = true;
            logger_.Write(
                "INFO", "xr_frame_pacing_active",
                "Duplicate stereo renders wait at most 20ms for the next "
                "OpenXR request; host loss always falls back.");
        }

        const ULONGLONG waitStart = GetTickCount64();
        const ULONGLONG deadline = waitStart + boundedTimeout;
        for (;;) {
            ServiceCaptureDuringPacingWaitLocked();
            if (ReadRenderRequestLocked(&snapshot) &&
                snapshot.frameId != previousFrameId) {
                *output = snapshot;
                const std::uint64_t waited =
                    static_cast<std::uint64_t>(
                        GetTickCount64() - waitStart);
                xrPacingWaitMaxMilliseconds_ = (std::max)(
                    xrPacingWaitMaxMilliseconds_, waited);
                ++xrPacingWaits_;
                if (xrPacingWaits_ % 300 == 0) {
                    std::ostringstream message;
                    message << "waits=300 max_wait_ms="
                            << xrPacingWaitMaxMilliseconds_
                            << " timeouts=" << xrPacingTimeouts_;
                    logger_.Write(
                        "INFO", "xr_frame_pacing", message.str());
                    xrPacingWaitMaxMilliseconds_ = 0;
                    xrPacingTimeouts_ = 0;
                }
                return TRUE;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                break;
            }
            const DWORD remaining = static_cast<DWORD>(
                (std::min)(deadline - now, 1ULL));
            const DWORD waitResult =
                WaitForSingleObject(renderRequestEvent_, remaining);
            if (waitResult == WAIT_FAILED) {
                break;
            }
        }
        ++xrPacingTimeouts_;
        return FALSE;
    }

private:
    void ServiceCaptureDuringPacingWaitLocked() noexcept {
        PollPending();
        if (device_ != nullptr && resourcesReady_ &&
            transferMode_ == TransferMode::CpuViaD3D9Ex &&
            !config_.syncCpuBridge) {
            ProcessCpuCaptureQueue(device_);
            PollPending();
        }
    }

    BOOL ReadRenderRequestLocked(
        FearVrRenderRequest* output) noexcept {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t before =
                ReadAtomic64(shared_->requestSequence);
            if ((before & 1ULL) != 0 || before == 0) {
                continue;
            }
            FearVrRenderRequest snapshot = shared_->request;
            MemoryBarrier();
            const std::uint64_t after =
                ReadAtomic64(shared_->requestSequence);
            if (before == after && (after & 1ULL) == 0 &&
                (snapshot.flags & FEARVR_RF_VALID) != 0) {
                snapshot.recenterGeneration = recenterGeneration_;
                if (config_.translationEnabled) {
                    snapshot.flags |= FEARVR_RF_TRANSLATION_ON;
                }
                if (comfortModeEnabled_) {
                    snapshot.flags |= FEARVR_RF_FLATSCREEN;
                }
                if (menuActive_) {
                    snapshot.flags |= FEARVR_RF_FLATSCREEN;
                }
                *output = snapshot;
                return TRUE;
            }
        }
        return FALSE;
    }

public:

    BOOL ReadInputState(FearVrInputState* output) noexcept {
        if (output == nullptr) {
            return FALSE;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc()) {
            return FALSE;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t before =
                ReadAtomic64(shared_->inputSequence);
            if ((before & 1ULL) != 0 || before == 0) {
                continue;
            }
            const FearVrInputState snapshot = shared_->input;
            MemoryBarrier();
            const std::uint64_t after =
                ReadAtomic64(shared_->inputSequence);
            if (before == after && (after & 1ULL) == 0 &&
                (snapshot.flags & FEARVR_IF_VALID) != 0) {
                *output = snapshot;
                return TRUE;
            }
        }
        return FALSE;
    }

    BOOL WriteHapticRequest(
        const FearVrHapticRequest* request) noexcept {
        if (request == nullptr ||
            (request->flags & FEARVR_HF_VALID) == 0 ||
            request->requestId == 0) {
            return FALSE;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc() || !hostConnected_) {
            return FALSE;
        }
        InterlockedIncrement64(Atomic64(shared_->hapticSequence));
        MemoryBarrier();
        shared_->haptic = *request;
        MemoryBarrier();
        InterlockedIncrement64(Atomic64(shared_->hapticSequence));
        return TRUE;
    }

    void BeginStereoEye(std::uint32_t eye) noexcept {
        if (eye >= FEARVR_EYE_COUNT) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        RestoreStereoRenderTarget();
        if (eye == FEARVR_EYE_LEFT) {
            if (stereoFrameReady_) {
                stereoAccepting_ = false;
                return;
            }
            stereoEyeCaptured_.fill(false);
            stereoFrameId_ = 0;
            stereoAccepting_ = true;
        }
        if (stereoAccepting_) {
            ActivateStereoRenderTarget(eye);
        }
    }

    void CaptureStereoEye(std::uint32_t eye) noexcept {
        if (eye >= FEARVR_EYE_COUNT || config_.sessionId == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stereoAccepting_ || device_ == nullptr ||
            !resourcesReady_ || !stereoCapture_[eye]) {
            RestoreStereoRenderTarget();
            return;
        }

        if (stereoRenderTargetActive_ &&
            stereoRenderTargetEye_ == eye) {
            bool captured = true;
            if (stereoRenderTarget_[eye].Get() !=
                stereoCapture_[eye].Get()) {
                const HRESULT result = device_->StretchRect(
                    stereoRenderTarget_[eye].Get(), nullptr,
                    stereoCapture_[eye].Get(), nullptr,
                    D3DTEXF_NONE);
                if (FAILED(result)) {
                    LogHresult(
                        "render_scale_msaa_resolve_failed", result);
                    captured = false;
                }
            }
            RestoreStereoRenderTarget();
            if (captured) {
                stereoEyeCaptured_[eye] = true;
            }
            return;
        }
        RestoreStereoRenderTarget();

        ComPtr<IDirect3DSurface9> backBuffer;
        HRESULT result = device_->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO,
            backBuffer.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult("stereo_get_backbuffer_failed", result);
            return;
        }
        D3DSURFACE_DESC description{};
        result = backBuffer->GetDesc(&description);
        if (FAILED(result) ||
            description.Width != sourceWidth_ ||
            description.Height != sourceHeight_) {
            logger_.Write(
                "WARN", "stereo_capture_size_changed",
                "Stereo capture deferred until resources are recreated.");
            return;
        }
        result = device_->StretchRect(
            backBuffer.Get(), nullptr, stereoCapture_[eye].Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("stereo_stage_copy_failed", result);
            return;
        }
        stereoEyeCaptured_[eye] = true;
    }

    void EndStereoFrame(std::uint64_t frameId) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        RestoreStereoRenderTarget();
        if (!stereoAccepting_) {
            return;
        }
        stereoAccepting_ = false;
        if (frameId == 0 ||
            !stereoEyeCaptured_[FEARVR_EYE_LEFT] ||
            !stereoEyeCaptured_[FEARVR_EYE_RIGHT]) {
            if (!stereoIncompleteLogged_) {
                logger_.Write(
                    "WARN", "stereo_frame_incomplete",
                    "Both eye captures are required; mono fallback remains active.");
                stereoIncompleteLogged_ = true;
            }
            stereoEyeCaptured_.fill(false);
            stereoFrameId_ = 0;
            return;
        }
        stereoFrameId_ = frameId;
        stereoFrameReady_ = true;
    }

private:
    struct PendingFrame {
        bool active{false};
        std::uint32_t slotIndex{0};
        std::uint64_t frameId{0};
        std::uint64_t generation{0};
    };

    bool IsSupersampledEyeTargetBound(
        IDirect3DDevice9* device) noexcept {
        if (!stereoRenderTargetActive_ || device != device_ ||
            stereoRenderTargetEye_ >= FEARVR_EYE_COUNT ||
            !stereoRenderTarget_[stereoRenderTargetEye_]) {
            return false;
        }
        ComPtr<IDirect3DSurface9> current;
        const HRESULT result = device->GetRenderTarget(
            0, current.ReleaseAndGetAddressOf());
        return SUCCEEDED(result) &&
            current.Get() ==
                stereoRenderTarget_[stereoRenderTargetEye_].Get();
    }

    bool ActivateStereoRenderTarget(
        std::uint32_t eye) noexcept {
        if (!stereoSupersamplingReady_ || device_ == nullptr ||
            eye >= FEARVR_EYE_COUNT ||
            !stereoRenderTarget_[eye] ||
            !stereoDepthStencil_) {
            return false;
        }

        HRESULT result = device_->GetRenderTarget(
            0, savedRenderTarget_.ReleaseAndGetAddressOf());
        if (FAILED(result) || !savedRenderTarget_) {
            LogHresult("render_scale_save_target_failed", result);
            return false;
        }
        result = device_->GetDepthStencilSurface(
            savedDepthStencil_.ReleaseAndGetAddressOf());
        if (FAILED(result) || !savedDepthStencil_) {
            savedRenderTarget_.Reset();
            LogHresult("render_scale_save_depth_failed", result);
            return false;
        }
        result = device_->GetViewport(&savedViewport_);
        if (FAILED(result)) {
            savedDepthStencil_.Reset();
            savedRenderTarget_.Reset();
            LogHresult("render_scale_save_viewport_failed", result);
            return false;
        }
        savedViewportValid_ = true;
        result = device_->GetScissorRect(&savedScissorRect_);
        if (SUCCEEDED(result)) {
            savedScissorValid_ = true;
        }

        result = device_->SetRenderTarget(
            0, stereoRenderTarget_[eye].Get());
        if (FAILED(result)) {
            LogHresult("render_scale_set_target_failed", result);
            RestoreStereoRenderTarget();
            return false;
        }
        stereoRenderTargetActive_ = true;
        stereoRenderTargetEye_ = eye;

        result = device_->SetDepthStencilSurface(
            stereoDepthStencil_.Get());
        if (FAILED(result)) {
            LogHresult("render_scale_set_depth_failed", result);
            RestoreStereoRenderTarget();
            return false;
        }

        D3DVIEWPORT9 viewport{};
        viewport.Width = width_;
        viewport.Height = height_;
        viewport.MinZ = savedViewport_.MinZ;
        viewport.MaxZ = savedViewport_.MaxZ;
        g_internalViewportStateChange = true;
        result = device_->SetViewport(&viewport);
        g_internalViewportStateChange = false;
        if (FAILED(result)) {
            LogHresult("render_scale_set_viewport_failed", result);
            RestoreStereoRenderTarget();
            return false;
        }
        return true;
    }

    void RestoreStereoRenderTarget() noexcept {
        if (!savedRenderTarget_ && !savedDepthStencil_ &&
            !savedViewportValid_) {
            stereoRenderTargetActive_ = false;
            return;
        }
        if (device_ != nullptr) {
            if (savedRenderTarget_) {
                const HRESULT result = device_->SetRenderTarget(
                    0, savedRenderTarget_.Get());
                if (FAILED(result)) {
                    LogHresult(
                        "render_scale_restore_target_failed", result);
                }
            }
            if (savedDepthStencil_) {
                const HRESULT result =
                    device_->SetDepthStencilSurface(
                        savedDepthStencil_.Get());
                if (FAILED(result)) {
                    LogHresult(
                        "render_scale_restore_depth_failed", result);
                }
            }
            if (savedViewportValid_) {
                g_internalViewportStateChange = true;
                const HRESULT result =
                    device_->SetViewport(&savedViewport_);
                g_internalViewportStateChange = false;
                if (FAILED(result)) {
                    LogHresult(
                        "render_scale_restore_viewport_failed", result);
                }
            }
            if (savedScissorValid_) {
                g_internalViewportStateChange = true;
                const HRESULT result =
                    device_->SetScissorRect(&savedScissorRect_);
                g_internalViewportStateChange = false;
                if (FAILED(result)) {
                    LogHresult(
                        "render_scale_restore_scissor_failed",
                        result);
                }
            }
        }
        savedRenderTarget_.Reset();
        savedDepthStencil_.Reset();
        savedViewport_ = {};
        savedViewportValid_ = false;
        savedScissorRect_ = {};
        savedScissorValid_ = false;
        stereoRenderTargetActive_ = false;
    }

    void ClearStereoFrame() noexcept {
        stereoEyeCaptured_.fill(false);
        stereoFrameId_ = 0;
        stereoFrameReady_ = false;
        stereoAccepting_ = false;
    }

    void IncrementRecenterGeneration() noexcept {
        ++recenterGeneration_;
        if (recenterGeneration_ == 0) {
            ++recenterGeneration_;
        }
    }

    void IncrementPanelRecenterGeneration() noexcept {
        if (shared_ == nullptr) {
            return;
        }
        LONG generation = InterlockedIncrement(
            Atomic32(shared_->panelRecenterGeneration));
        if (generation == 0) {
            InterlockedIncrement(
                Atomic32(shared_->panelRecenterGeneration));
        }
    }

    void PollStereoToggle() noexcept {
        if (!config_.stereoToggleAllowed) {
            return;
        }
        StereoToggleCallback callback = nullptr;
        BOOL enabled = FALSE;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool keyDown =
                (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (keyDown && !stereoKeyWasDown_) {
                config_.stereoEnabled = !config_.stereoEnabled;
                ClearStereoFrame();
                if (shared_ != nullptr &&
                    !config_.stereoEnabled) {
                    InterlockedAnd(
                        AtomicFlags(*shared_),
                        static_cast<LONG>(
                            ~FEARVR_BF_STEREO_ACTIVE));
                }
                callback = stereoToggleCallback_;
                enabled = config_.stereoEnabled ? TRUE : FALSE;
                changed = true;
                logger_.Write(
                    "INFO", "stereo_toggle",
                    config_.stereoEnabled
                        ? "Native stereo enabled with F8."
                        : "Native stereo disabled with F8; mono fallback active.");
            }
            stereoKeyWasDown_ = keyDown;

            const bool recenterKeyDown =
                (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (recenterKeyDown && !recenterKeyWasDown_) {
                const bool flatView =
                    menuActive_ || comfortModeEnabled_ ||
                    !config_.stereoEnabled;
                if (flatView) {
                    IncrementPanelRecenterGeneration();
                    logger_.Write(
                        "INFO", "panel_recenter_requested",
                        "F9 requested a new 2D panel anchor.");
                } else {
                    logger_.Write(
                        "INFO", "world_recenter_ignored",
                        "F9 has no recenter function in the 3D world.");
                }
            }
            recenterKeyWasDown_ = recenterKeyDown;

            const bool comfortKeyDown =
                (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
            if (comfortKeyDown && !comfortKeyWasDown_) {
                comfortModeEnabled_ = !comfortModeEnabled_;
                ClearStereoFrame();
                if (shared_ != nullptr) {
                    InterlockedAnd(
                        AtomicFlags(*shared_),
                        static_cast<LONG>(
                            ~FEARVR_BF_STEREO_ACTIVE));
                }
                if (!comfortModeEnabled_) {
                    IncrementRecenterGeneration();
                }
                logger_.Write(
                    "INFO", "comfort_mode_toggle",
                    comfortModeEnabled_
                        ? "World-locked comfort panel enabled with F10."
                        : "Comfort panel disabled with F10; stereo resumes.");
            }
            comfortKeyWasDown_ = comfortKeyDown;
        }
        if (changed && callback != nullptr) {
            callback(enabled);
        }
    }

    bool EnsureIpc() noexcept {
        if (shared_ != nullptr) {
            return true;
        }

        const std::wstring mappingName =
            MakeIpcObjectName(config_.sessionId, L"Mapping");
        mapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(FearVrSharedHeader)),
            mappingName.c_str());
        if (mapping_ == nullptr) {
            LogWin32("mapping_create_failed", GetLastError());
            return false;
        }
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        shared_ = static_cast<FearVrSharedHeader*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                          sizeof(FearVrSharedHeader)));
        if (shared_ == nullptr) {
            LogWin32("mapping_view_failed", GetLastError());
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        if (existed) {
            if (!IsProtocolHeaderValid(*shared_)) {
                logger_.Write("ERROR", "protocol_mismatch",
                              "Existing mapping has incompatible header.");
                InterlockedOr(AtomicFlags(*shared_),
                              FEARVR_BF_PROTOCOL_ERROR);
                return false;
            }
        } else {
            InitializeProtocolHeader(*shared_);
        }

        const std::wstring frameReadyName =
            MakeIpcObjectName(config_.sessionId, L"FrameReady");
        const std::wstring consumedName =
            MakeIpcObjectName(config_.sessionId, L"SlotConsumed");
        const std::wstring renderRequestName =
            MakeIpcObjectName(config_.sessionId, L"RenderRequest");
        frameReadyEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, frameReadyName.c_str());
        slotConsumedEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, consumedName.c_str());
        renderRequestEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, renderRequestName.c_str());
        if (frameReadyEvent_ == nullptr || slotConsumedEvent_ == nullptr ||
            renderRequestEvent_ == nullptr) {
            LogWin32("event_create_failed", GetLastError());
            return false;
        }

        shared_->gameProcessId = GetCurrentProcessId();
        InterlockedExchange(
            Atomic32(shared_->fovScalePercent),
            static_cast<LONG>(fovScalePercent_));
        InterlockedOr(AtomicFlags(*shared_), FEARVR_BF_GAME_READY);
        logger_.Write("INFO", "ipc_created",
                      "Named mapping and bounded ring ready.");
        return true;
    }

    void EnsureDeviceMetadata(IDirect3DDevice9* device) noexcept {
        if (deviceMetadataReady_ && device_ == device) {
            return;
        }
        if (device_ != device) {
            ReleaseResources();
            device_ = device;
        }

        D3DDEVICE_CREATION_PARAMETERS creation{};
        if (FAILED(device->GetCreationParameters(&creation))) {
            return;
        }
        ComPtr<IDirect3D9> direct3D;
        if (FAILED(device->GetDirect3D(
                direct3D.ReleaseAndGetAddressOf()))) {
            return;
        }
        bool found = false;
        ComPtr<IDirect3D9Ex> direct3DEx;
        if (SUCCEEDED(direct3D.As(&direct3DEx))) {
            LUID adapterLuid{};
            if (SUCCEEDED(direct3DEx->GetAdapterLUID(
                    creation.AdapterOrdinal, &adapterLuid))) {
                gameAdapterLuid_ = PackLuid(
                    static_cast<std::uint32_t>(adapterLuid.HighPart),
                    adapterLuid.LowPart);
                found = true;
            }
        }

        // Auf Hybrid-GPUs kann der D3D9-Renderadapter keine eigene Ausgabe
        // besitzen. Geräte-IDs sind dort zuverlässiger als der Monitor.
        if (!found) {
            D3DADAPTER_IDENTIFIER9 d3d9Identifier{};
            if (SUCCEEDED(direct3D->GetAdapterIdentifier(
                    creation.AdapterOrdinal, 0, &d3d9Identifier))) {
                ComPtr<IDXGIFactory1> factory;
                if (SUCCEEDED(CreateDXGIFactory1(
                        IID_PPV_ARGS(
                            factory.ReleaseAndGetAddressOf())))) {
                    for (UINT adapterIndex = 0;; ++adapterIndex) {
                        ComPtr<IDXGIAdapter1> adapter;
                        if (factory->EnumAdapters1(
                                adapterIndex,
                                adapter.ReleaseAndGetAddressOf()) ==
                            DXGI_ERROR_NOT_FOUND) {
                            break;
                        }
                        DXGI_ADAPTER_DESC1 description{};
                        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                            description.VendorId ==
                                d3d9Identifier.VendorId &&
                            description.DeviceId ==
                                d3d9Identifier.DeviceId &&
                            description.SubSysId ==
                                d3d9Identifier.SubSysId &&
                            description.Revision ==
                                d3d9Identifier.Revision) {
                            gameAdapterLuid_ = PackLuid(
                                static_cast<std::uint32_t>(
                                    description.AdapterLuid.HighPart),
                                description.AdapterLuid.LowPart);
                            found = true;
                            break;
                        }
                    }
                }
            }
        }

        // Klassische IDirect3D9-Objekte besitzen keine LUID-Abfrage.
        // Dort bleibt die Monitor-zu-DXGI-Zuordnung der sichere Fallback.
        if (!found) {
            const HMONITOR monitor =
                direct3D->GetAdapterMonitor(creation.AdapterOrdinal);
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(
                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
                logger_.Write("ERROR", "adapter_luid_failed",
                              "CreateDXGIFactory1 failed.");
                return;
            }

            for (UINT adapterIndex = 0; !found; ++adapterIndex) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(
                        adapterIndex,
                        adapter.ReleaseAndGetAddressOf()) ==
                    DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                for (UINT outputIndex = 0;; ++outputIndex) {
                    ComPtr<IDXGIOutput> output;
                    const HRESULT enumResult = adapter->EnumOutputs(
                        outputIndex, output.ReleaseAndGetAddressOf());
                    if (enumResult == DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    if (FAILED(enumResult)) {
                        continue;
                    }
                    DXGI_OUTPUT_DESC outputDescription{};
                    if (SUCCEEDED(output->GetDesc(&outputDescription)) &&
                        outputDescription.Monitor == monitor) {
                        DXGI_ADAPTER_DESC1 adapterDescription{};
                        if (SUCCEEDED(adapter->GetDesc1(
                                &adapterDescription))) {
                            gameAdapterLuid_ = PackLuid(
                                static_cast<std::uint32_t>(
                                    adapterDescription.AdapterLuid.HighPart),
                                adapterDescription.AdapterLuid.LowPart);
                            found = true;
                        }
                        break;
                    }
                }
            }
        }
        if (!found) {
            logger_.Write("ERROR", "adapter_luid_failed",
                          "No DXGI adapter matched the D3D9 monitor.");
            return;
        }

        shared_->gameAdapterLuid = gameAdapterLuid_;
        deviceMetadataReady_ = true;
        std::ostringstream message;
        message << "luid=0x" << std::hex << std::uppercase
                << LuidHigh(gameAdapterLuid_) << ':'
                << LuidLow(gameAdapterLuid_);
        logger_.Write("INFO", "d3d9_adapter", message.str());
    }

    void UpdateAdapterMatch() noexcept {
        if (!deviceMetadataReady_ || shared_->hostAdapterLuid == 0) {
            return;
        }
        if (shared_->hostAdapterLuid == gameAdapterLuid_) {
            InterlockedOr(AtomicFlags(*shared_), FEARVR_BF_ADAPTER_MATCH);
            if (!adapterMatchLogged_) {
                logger_.Write("INFO", "adapter_match",
                              "D3D9 and OpenXR adapter LUIDs match.");
                adapterMatchLogged_ = true;
            }
        } else {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(~FEARVR_BF_ADAPTER_MATCH));
            if (!adapterMismatchLogged_) {
                std::ostringstream message;
                message << "game=0x" << std::hex << std::uppercase
                        << gameAdapterLuid_ << " host=0x"
                        << shared_->hostAdapterLuid;
                logger_.Write("ERROR", "adapter_mismatch", message.str());
                adapterMismatchLogged_ = true;
            }
        }
    }

    void UpdateHostConnection() noexcept {
        const std::uint64_t heartbeat =
            ReadAtomic64(shared_->hostHeartbeat);
        const ULONGLONG now = GetTickCount64();
        if (heartbeat != lastHostHeartbeat_) {
            lastHostHeartbeat_ = heartbeat;
            lastHostHeartbeatTick_ = now;
        }
        const bool connected =
            heartbeat != 0 &&
            (shared_->bridgeFlags & FEARVR_BF_HOST_READY) != 0 &&
            now - lastHostHeartbeatTick_ <= 2000;
        if (connected != hostConnected_) {
            hostConnected_ = connected;
            logger_.Write(connected ? "INFO" : "WARN",
                          connected ? "host_connected"
                                    : "host_disconnected",
                          connected
                              ? "Host heartbeat active."
                              : "Host heartbeat timed out; flat screen continues.");
            if (!connected) {
                RecoverSlotsAfterHostLoss();
            }
        }
    }

    void RecoverSlotsAfterHostLoss() noexcept {
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            for (std::uint32_t slotIndex = 0;
                 slotIndex < FEARVR_SLOTS_PER_EYE; ++slotIndex) {
                FearVrSlot& slot = shared_->slot[eye][slotIndex];
                if (slot.state == FEARVR_SLOT_READY ||
                    slot.state == FEARVR_SLOT_CONSUMING) {
                    InterlockedExchange(AtomicState(slot),
                                        FEARVR_SLOT_EMPTY);
                }
            }
        }
    }

    bool CreateSharedSlots(IDirect3DDevice9* resourceDevice, UINT width,
                           UINT height) noexcept {
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            for (std::uint32_t slotIndex = 0;
                 slotIndex < FEARVR_SLOTS_PER_EYE; ++slotIndex) {
                SlotResource& resource = resources_[eye][slotIndex];
                HANDLE sharedHandle = nullptr;
                HRESULT result = resourceDevice->CreateTexture(
                    width, height, 1, D3DUSAGE_RENDERTARGET,
                    D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                    resource.texture.ReleaseAndGetAddressOf(),
                    &sharedHandle);
                if (FAILED(result) || sharedHandle == nullptr) {
                    LogHresult("shared_texture_create_failed", result);
                    return false;
                }
                resource.sharedHandle = sharedHandle;
                result = resource.texture->GetSurfaceLevel(
                    0, resource.surface.ReleaseAndGetAddressOf());
                if (FAILED(result)) {
                    LogHresult("shared_surface_failed", result);
                    return false;
                }
                result = resourceDevice->CreateQuery(
                    D3DQUERYTYPE_EVENT,
                    resource.completion.ReleaseAndGetAddressOf());
                if (FAILED(result) || !resource.completion) {
                    LogHresult("d3d9_query_create_failed", result);
                    return false;
                }

                FearVrSlot& slot = shared_->slot[eye][slotIndex];
                slot.sharedHandle = static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(sharedHandle));
                slot.frameId = 0;
                slot.width = width;
                slot.height = height;
                slot.format = FEARVR_FMT_B8G8R8A8;
                slot.generation = 0;
                InterlockedExchange(AtomicState(slot),
                                    FEARVR_SLOT_EMPTY);
            }
        }
        return true;
    }

    bool CreateStereoCaptureSurfaces(IDirect3DDevice9* gameDevice,
                                     UINT width,
                                     UINT height) noexcept {
        // Render-Target-*Texturen* statt reiner Surfaces: Der GPU-Kompositor
        // muss die beiden Weltbilder in einem Shader abtasten können, und für
        // StretchRect und GetRenderTargetData ist Ebene 0 einer solchen Textur
        // dasselbe wie ein Render-Target-Surface.
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            HRESULT result = gameDevice->CreateTexture(
                width, height, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                stereoCaptureTexture_[eye].ReleaseAndGetAddressOf(),
                nullptr);
            if (FAILED(result)) {
                LogHresult("stereo_capture_create_failed", result);
                return false;
            }
            result = stereoCaptureTexture_[eye]->GetSurfaceLevel(
                0, stereoCapture_[eye].ReleaseAndGetAddressOf());
            if (FAILED(result)) {
                LogHresult("stereo_capture_surface_failed", result);
                return false;
            }
        }
        return true;
    }

    bool CreateStereoRenderTargets(IDirect3DDevice9* gameDevice,
                                   UINT width,
                                   UINT height) noexcept {
        stereoSupersamplingReady_ = false;
        if (effectiveRenderScalePercent_ <=
            kRenderScaleMinimumPercent) {
            return true;
        }

        ComPtr<IDirect3DSurface9> originalRenderTarget;
        HRESULT result = gameDevice->GetRenderTarget(
            0, originalRenderTarget.ReleaseAndGetAddressOf());
        if (FAILED(result) || !originalRenderTarget) {
            LogHresult("render_scale_get_target_failed", result);
            return true;
        }
        D3DSURFACE_DESC renderDescription{};
        result = originalRenderTarget->GetDesc(&renderDescription);
        if (FAILED(result)) {
            LogHresult("render_scale_target_desc_failed", result);
            return true;
        }

        ComPtr<IDirect3DSurface9> originalDepthStencil;
        result = gameDevice->GetDepthStencilSurface(
            originalDepthStencil.ReleaseAndGetAddressOf());
        if (FAILED(result) || !originalDepthStencil) {
            LogHresult("render_scale_get_depth_failed", result);
            return true;
        }
        D3DSURFACE_DESC depthDescription{};
        result = originalDepthStencil->GetDesc(&depthDescription);
        if (FAILED(result)) {
            LogHresult("render_scale_depth_desc_failed", result);
            return true;
        }

        const D3DMULTISAMPLE_TYPE multisample =
            renderDescription.MultiSampleType;
        const DWORD multisampleQuality =
            renderDescription.MultiSampleQuality;
        if (depthDescription.MultiSampleType != multisample ||
            depthDescription.MultiSampleQuality !=
                multisampleQuality) {
            logger_.Write(
                "WARN", "render_scale_msaa_mismatch",
                "Retail render target and depth buffer use different "
                "MSAA settings; supersampling remains disabled.");
            return true;
        }

        result = gameDevice->CreateDepthStencilSurface(
            width, height, depthDescription.Format, multisample,
            multisampleQuality, TRUE,
            stereoDepthStencil_.ReleaseAndGetAddressOf(), nullptr);
        if (FAILED(result) || !stereoDepthStencil_) {
            LogHresult("render_scale_depth_create_failed", result);
            return true;
        }

        for (std::uint32_t eye = 0;
             eye < FEARVR_EYE_COUNT; ++eye) {
            if (multisample == D3DMULTISAMPLE_NONE) {
                stereoRenderTarget_[eye] = stereoCapture_[eye];
                continue;
            }
            result = gameDevice->CreateRenderTarget(
                width, height, D3DFMT_A8R8G8B8, multisample,
                multisampleQuality, FALSE,
                stereoRenderTarget_[eye].ReleaseAndGetAddressOf(),
                nullptr);
            if (FAILED(result) || !stereoRenderTarget_[eye]) {
                LogHresult(
                    "render_scale_msaa_target_create_failed", result);
                for (auto& target : stereoRenderTarget_) {
                    target.Reset();
                }
                stereoDepthStencil_.Reset();
                return true;
            }
        }

        stereoSupersamplingReady_ = true;
        std::ostringstream message;
        message << "source=" << sourceWidth_ << 'x' << sourceHeight_
                << " target=" << width << 'x' << height
                << " percent=" << effectiveRenderScalePercent_
                << " msaa=" << static_cast<unsigned>(multisample)
                << " quality=" << multisampleQuality;
        logger_.Write(
            "INFO", "render_scale_ready", message.str());
        return true;
    }

    bool CreateCpuCaptureQueue(IDirect3DDevice9* gameDevice,
                               UINT width, UINT height) noexcept {
        for (CpuCaptureFrame& frame : cpuCaptureQueue_) {
            for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
                HRESULT result = gameDevice->CreateTexture(
                    width, height, 1, D3DUSAGE_RENDERTARGET,
                    D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                    frame.texture[eye].ReleaseAndGetAddressOf(), nullptr);
                if (FAILED(result)) {
                    LogHresult("cpu_capture_queue_create_failed", result);
                    return false;
                }
                result = frame.texture[eye]->GetSurfaceLevel(
                    0, frame.surface[eye].ReleaseAndGetAddressOf());
                if (FAILED(result)) {
                    LogHresult("cpu_capture_queue_surface_failed", result);
                    return false;
                }
            }
            HRESULT result = gameDevice->CreateQuery(
                D3DQUERYTYPE_EVENT,
                frame.completion.ReleaseAndGetAddressOf());
            if (FAILED(result) || !frame.completion) {
                LogHresult("cpu_capture_queue_query_failed", result);
                return false;
            }
        }
        cpuCaptureRead_ = 0;
        cpuCaptureWrite_ = 0;
        cpuCaptureCount_ = 0;
        return true;
    }

    bool CreateCpuInteropResources(IDirect3DDevice9* gameDevice,
                                   UINT width, UINT height) noexcept {
        using Direct3DCreate9ExFunction =
            HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
        const auto createDirect3DEx =
            ResolveSystemD3D9<Direct3DCreate9ExFunction>(
                "Direct3DCreate9Ex");
        if (createDirect3DEx == nullptr) {
            logger_.Write("ERROR", "cpu_bridge_d3d9ex_missing",
                          "System Direct3DCreate9Ex is unavailable.");
            return false;
        }

        IDirect3D9Ex* direct3DEx = nullptr;
        HRESULT result =
            createDirect3DEx(D3D_SDK_VERSION, &direct3DEx);
        bridgeDirect3DEx_.Attach(direct3DEx);
        if (FAILED(result) || !bridgeDirect3DEx_) {
            LogHresult("cpu_bridge_d3d9ex_failed", result);
            return false;
        }

        UINT bridgeAdapter = D3DADAPTER_DEFAULT;
        bool adapterFound = false;
        for (UINT index = 0;
             index < bridgeDirect3DEx_->GetAdapterCount(); ++index) {
            LUID luid{};
            if (SUCCEEDED(bridgeDirect3DEx_->GetAdapterLUID(
                    index, &luid)) &&
                PackLuid(
                    static_cast<std::uint32_t>(luid.HighPart),
                    luid.LowPart) == gameAdapterLuid_) {
                bridgeAdapter = index;
                adapterFound = true;
                break;
            }
        }
        if (!adapterFound) {
            logger_.Write(
                "ERROR", "cpu_bridge_adapter_failed",
                "No D3D9Ex adapter matched the game adapter LUID.");
            return false;
        }

        companionWindow_ = CreateWindowExW(
            0, L"STATIC", L"FearVrD3D9CpuBridge", WS_OVERLAPPED,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr),
            nullptr);
        if (companionWindow_ == nullptr) {
            LogWin32("cpu_bridge_window_failed", GetLastError());
            return false;
        }

        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = companionWindow_;
        parameters.BackBufferWidth = 32;
        parameters.BackBufferHeight = 32;
        parameters.BackBufferFormat = D3DFMT_UNKNOWN;
        result = bridgeDirect3DEx_->CreateDeviceEx(
            bridgeAdapter, D3DDEVTYPE_HAL, companionWindow_,
            D3DCREATE_HARDWARE_VERTEXPROCESSING |
                D3DCREATE_FPU_PRESERVE,
            &parameters, nullptr,
            bridgeDeviceEx_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            result = bridgeDirect3DEx_->CreateDeviceEx(
                bridgeAdapter, D3DDEVTYPE_HAL, companionWindow_,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                    D3DCREATE_FPU_PRESERVE,
                &parameters, nullptr,
                bridgeDeviceEx_.ReleaseAndGetAddressOf());
        }
        if (FAILED(result) || !bridgeDeviceEx_) {
            LogHresult("cpu_bridge_device_failed", result);
            return false;
        }

        result = gameDevice->CreateTexture(
            width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, gameCaptureTexture_.ReleaseAndGetAddressOf(),
            nullptr);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_capture_failed", result);
            return false;
        }
        result = gameCaptureTexture_->GetSurfaceLevel(
            0, gameCapture_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult("cpu_bridge_capture_surface_failed", result);
            return false;
        }
        if (!CreateCpuCaptureQueue(gameDevice, width, height)) {
            return false;
        }
        result = gameDevice->CreateOffscreenPlainSurface(
            width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
            gameReadback_.ReleaseAndGetAddressOf(), nullptr);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_readback_failed", result);
            return false;
        }
        if (config_.stereoHudEnabled) {
            result = gameDevice->CreateOffscreenPlainSurface(
                width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                rightWorldReadback_.ReleaseAndGetAddressOf(), nullptr);
            if (FAILED(result)) {
                LogHresult("stereo_hud_right_readback_failed", result);
                return false;
            }
            result = gameDevice->CreateOffscreenPlainSurface(
                width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                presentedReadback_.ReleaseAndGetAddressOf(), nullptr);
            if (FAILED(result)) {
                LogHresult("stereo_hud_present_readback_failed", result);
                return false;
            }
        }
        result = bridgeDeviceEx_->CreateOffscreenPlainSurface(
            width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
            bridgeUpload_.ReleaseAndGetAddressOf(), nullptr);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_upload_failed", result);
            return false;
        }
        if (!CreateSharedSlots(bridgeDeviceEx_.Get(), width, height)) {
            return false;
        }
        transferMode_ = TransferMode::CpuViaD3D9Ex;
        InterlockedOr(AtomicFlags(*shared_), FEARVR_BF_CPU_FALLBACK);
        if (config_.stereoHudEnabled) {
            logger_.Write(
                "INFO", "stereo_hud_ready",
                "Post-world HUD compositing enabled for CPU transfer.");
        }
        return true;
    }

    bool EnsureResources(IDirect3DDevice9* device, UINT width,
                         UINT height) noexcept {
        if (resourcesReady_ && sourceWidth_ == width &&
            sourceHeight_ == height && device_ == device) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < nextResourceRetryTick_) {
            return false;
        }

        ReleaseResources();
        device_ = device;
        sourceWidth_ = width;
        sourceHeight_ = height;

        D3DCAPS9 capabilities{};
        if (FAILED(device->GetDeviceCaps(&capabilities))) {
            capabilities.MaxTextureWidth = width;
            capabilities.MaxTextureHeight = height;
        }
        const RenderScaleSize renderSize =
            CalculateRenderScaleSize(
                width, height, config_.renderScalePercent,
                capabilities.MaxTextureWidth,
                capabilities.MaxTextureHeight);
        width_ = renderSize.width == 0 ? width : renderSize.width;
        height_ = renderSize.height == 0 ? height : renderSize.height;
        effectiveRenderScalePercent_ =
            renderSize.width == 0
                ? kRenderScaleMinimumPercent
                : renderSize.percent;

        ComPtr<IDirect3DDevice9Ex> deviceEx;
        bool created = false;
        if (SUCCEEDED(device->QueryInterface(
                IID_PPV_ARGS(deviceEx.ReleaseAndGetAddressOf())))) {
            created = CreateSharedSlots(device, width_, height_);
            if (created) {
                transferMode_ = TransferMode::DirectShared;
            }
        } else {
            created =
                CreateCpuInteropResources(device, width_, height_);
        }
        if (created) {
            created =
                CreateStereoCaptureSurfaces(
                    device, width_, height_);
        }
        if (created) {
            created = CreateStereoRenderTargets(
                device, width_, height_);
        }
        if (created && config_.stereoHudEnabled &&
            !config_.disableGpuHud) {
            // Scheitert der Kompositor, bleibt das HUD trotzdem: dann mischt
            // wieder die CPU. Kein Grund, den ganzen Bildpfad aufzugeben.
            hudCompositor_.Initialize(
                device, width_, height_, logger_);
        }
        if (!created) {
            ReleaseResources();
            nextResourceRetryTick_ = now + 1000;
            return false;
        }

        nextResourceRetryTick_ = 0;
        resourcesReady_ = true;
        InterlockedOr(AtomicFlags(*shared_),
                      FEARVR_BF_SHARED_SUPPORTED);
        std::ostringstream message;
        message << "source=" << sourceWidth_ << 'x'
                << sourceHeight_ << " size=" << width_ << 'x'
                << height_ << " render_scale="
                << effectiveRenderScalePercent_ << "%"
                << " format=B8G8R8A8 slots="
                << FEARVR_SLOTS_PER_EYE << "x2 path="
                << (transferMode_ == TransferMode::DirectShared
                        ? "direct"
                        : "cpu_d3d9ex");
        logger_.Write(
            transferMode_ == TransferMode::DirectShared
                ? "INFO"
                : "WARN",
            "shared_resources", message.str());
        return true;
    }

    bool CopyFrameDirect(IDirect3DDevice9* device,
                         IDirect3DSurface9* backBuffer,
                         std::uint32_t slotIndex) noexcept {
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            SlotResource& resource = resources_[eye][slotIndex];
            HRESULT result = device->StretchRect(
                backBuffer, nullptr, resource.surface.Get(), nullptr,
                D3DTEXF_NONE);
            if (FAILED(result)) {
                LogHresult("stretch_rect_failed", result);
                return false;
            }
            result = resource.completion->Issue(D3DISSUE_END);
            if (FAILED(result)) {
                LogHresult("d3d9_query_issue_failed", result);
                return false;
            }
        }
        return true;
    }

    bool CopyStereoFrameDirect(IDirect3DDevice9* device,
                               std::uint32_t slotIndex) noexcept {
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            SlotResource& resource = resources_[eye][slotIndex];
            HRESULT result = device->StretchRect(
                stereoCapture_[eye].Get(), nullptr,
                resource.surface.Get(), nullptr, D3DTEXF_NONE);
            if (FAILED(result)) {
                LogHresult("stereo_stretch_rect_failed", result);
                return false;
            }
            result = resource.completion->Issue(D3DISSUE_END);
            if (FAILED(result)) {
                LogHresult("stereo_query_issue_failed", result);
                return false;
            }
        }
        return true;
    }

    bool StageSurfaceViaCpu(IDirect3DDevice9* device,
                            IDirect3DSurface9* sourceSurface) noexcept {
        HRESULT result = device->GetRenderTargetData(
            sourceSurface, gameReadback_.Get());
        if (FAILED(result)) {
            LogHresult("cpu_bridge_readback_copy_failed", result);
            return false;
        }

        D3DLOCKED_RECT source{};
        D3DLOCKED_RECT destination{};
        result = gameReadback_->LockRect(
            &source, nullptr, D3DLOCK_READONLY);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_readback_lock_failed", result);
            return false;
        }
        result = bridgeUpload_->LockRect(&destination, nullptr, 0);
        if (FAILED(result)) {
            gameReadback_->UnlockRect();
            LogHresult("cpu_bridge_upload_lock_failed", result);
            return false;
        }

        const std::size_t rowBytes =
            static_cast<std::size_t>(width_) * 4U;
        auto* sourceBytes =
            static_cast<const std::uint8_t*>(source.pBits);
        auto* destinationBytes =
            static_cast<std::uint8_t*>(destination.pBits);
        for (UINT row = 0; row < height_; ++row) {
            std::memcpy(destinationBytes, sourceBytes, rowBytes);
            sourceBytes += source.Pitch;
            destinationBytes += destination.Pitch;
        }
        bridgeUpload_->UnlockRect();
        gameReadback_->UnlockRect();
        return true;
    }

    bool UploadCpuSurface(std::uint32_t eye,
                          std::uint32_t slotIndex) noexcept {
        SlotResource& resource = resources_[eye][slotIndex];
        HRESULT result = bridgeDeviceEx_->UpdateSurface(
            bridgeUpload_.Get(), nullptr, resource.surface.Get(), nullptr);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_update_failed", result);
            return false;
        }
        result = resource.completion->Issue(D3DISSUE_END);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_query_failed", result);
            return false;
        }
        return true;
    }

    bool QueueCpuCapture(IDirect3DDevice9* device,
                         IDirect3DSurface9* backBuffer,
                         bool stereo) noexcept {
        if (cpuCaptureCount_ == kCpuCaptureQueueSize) {
            ++cpuCaptureQueueDrops_;
            if (stereo) {
                ClearStereoFrame();
            }
            return false;
        }

        CpuCaptureFrame& frame = cpuCaptureQueue_[cpuCaptureWrite_];
        if (frame.active || !frame.completion) {
            ++cpuCaptureQueueDrops_;
            if (stereo) {
                ClearStereoFrame();
            }
            return false;
        }

        std::array<IDirect3DSurface9*, FEARVR_EYE_COUNT> source{
            backBuffer, backBuffer};
        bool flatPanel = false;
        bool gpuHudComposited = false;
        std::uint64_t changedPixels = 0;
        const std::uint64_t totalPixels =
            static_cast<std::uint64_t>(width_) * height_;

        if (stereo) {
            if (config_.stereoHudEnabled && hudCompositor_.ready()) {
                HRESULT result = device->StretchRect(
                    backBuffer, nullptr, gameCapture_.Get(), nullptr,
                    D3DTEXF_NONE);
                if (FAILED(result)) {
                    LogHresult(
                        "async_stereo_hud_present_stretch_failed", result);
                    ClearStereoFrame();
                    return false;
                }
                changedPixels = static_cast<std::uint64_t>(
                    hudCompositor_.coverageRatio() *
                        static_cast<double>(totalPixels) + 0.5);
                flatPanel = menuActive_;
                const bool composite =
                    !flatPanel &&
                    IsSafePostWorldCoverage(changedPixels, totalPixels);
                IDirect3DTexture9* const eyeWorld[FEARVR_EYE_COUNT] = {
                    stereoCaptureTexture_[FEARVR_EYE_LEFT].Get(),
                    stereoCaptureTexture_[FEARVR_EYE_RIGHT].Get()};
                if (!hudCompositor_.Compose(
                        device, gameCaptureTexture_.Get(),
                        stereoCaptureTexture_[FEARVR_EYE_RIGHT].Get(),
                        eyeWorld, composite, flatPanel, logger_)) {
                    ClearStereoFrame();
                    return false;
                }
                for (std::uint32_t eye = 0;
                     eye < FEARVR_EYE_COUNT; ++eye) {
                    source[eye] = hudCompositor_.CompositeSurface(eye);
                }
                gpuHudComposited = true;
            } else {
                for (std::uint32_t eye = 0;
                     eye < FEARVR_EYE_COUNT; ++eye) {
                    source[eye] = stereoCapture_[eye].Get();
                }
            }
        }

        const std::uint32_t sourceCount =
            stereo ? FEARVR_EYE_COUNT : 1u;
        for (std::uint32_t eye = 0; eye < sourceCount; ++eye) {
            const HRESULT result = device->StretchRect(
                source[eye], nullptr, frame.surface[eye].Get(), nullptr,
                D3DTEXF_NONE);
            if (FAILED(result)) {
                LogHresult("cpu_capture_queue_stretch_failed", result);
                if (stereo) {
                    ClearStereoFrame();
                }
                return false;
            }
        }
        const HRESULT queryResult =
            frame.completion->Issue(D3DISSUE_END);
        if (FAILED(queryResult)) {
            LogHresult("cpu_capture_queue_issue_failed", queryResult);
            if (stereo) {
                ClearStereoFrame();
            }
            return false;
        }

        frame.stereo = stereo;
        frame.flatPanel = flatPanel;
        frame.renderModeGeneration = renderModeGeneration_;
        frame.frameId = stereo ? stereoFrameId_ : ++frameId_;
        if (stereo && frame.frameId > frameId_) {
            frameId_ = frame.frameId;
        }
        frame.active = true;
        cpuCaptureWrite_ =
            (cpuCaptureWrite_ + 1) % kCpuCaptureQueueSize;
        ++cpuCaptureCount_;
        ++cpuCaptureQueued_;

        if (gpuHudComposited) {
            ++stereoHudFrames_;
            if (stereoHudFrames_ == 1 ||
                stereoHudFrames_ % 300 == 0) {
                std::ostringstream message;
                message << "changed_pixels=" << changedPixels
                        << " coverage_percent="
                        << (totalPixels == 0
                                ? 0
                                : changedPixels * 100u / totalPixels)
                        << " mode="
                        << (flatPanel ? "flat_panel" : "raised_hud")
                        << " path=gpu_async";
                logger_.Write(
                    "INFO", "stereo_hud_async_staged", message.str());
            }
        }
        if (stereo) {
            ClearStereoFrame();
        }
        return true;
    }

    void RetireCpuCaptureHead() noexcept {
        CpuCaptureFrame& frame =
            cpuCaptureQueue_[cpuCaptureRead_];
        frame.active = false;
        cpuCaptureRead_ =
            (cpuCaptureRead_ + 1) % kCpuCaptureQueueSize;
        --cpuCaptureCount_;
    }

    void ProcessCpuCaptureQueue(
        IDirect3DDevice9* device) noexcept {
        while (cpuCaptureCount_ != 0) {
            CpuCaptureFrame& frame =
                cpuCaptureQueue_[cpuCaptureRead_];
            if (!frame.active || !frame.completion) {
                RetireCpuCaptureHead();
                continue;
            }

            const HRESULT queryResult =
                frame.completion->GetData(nullptr, 0, 0);
            if (queryResult == S_FALSE) {
                ++cpuCaptureQueryNotReady_;
                return;
            }
            if (FAILED(queryResult)) {
                LogHresult(
                    "cpu_capture_queue_getdata_failed", queryResult);
                RetireCpuCaptureHead();
                continue;
            }

            // Do not spend a GPU->CPU readback and D3D9Ex upload on an old
            // completed frame when a newer completed frame is already
            // available. Commands on one D3D9 device complete in order, so
            // probing the next queue entry is sufficient and repeating this
            // loop converges on the newest completed capture.
            if (cpuCaptureCount_ > 1) {
                const std::size_t nextIndex =
                    (cpuCaptureRead_ + 1) % kCpuCaptureQueueSize;
                CpuCaptureFrame& next =
                    cpuCaptureQueue_[nextIndex];
                if (next.active && next.completion &&
                    next.completion->GetData(nullptr, 0, 0) == S_OK) {
                    RetireCpuCaptureHead();
                    ++cpuCaptureStaleDrops_;
                    continue;
                }
            }

            // A menu transition invalidates the image but never waits for it.
            // Retire it only after the GPU query completes so its surfaces
            // cannot be overwritten while commands still reference them.
            if (frame.renderModeGeneration != renderModeGeneration_) {
                RetireCpuCaptureHead();
                ++cpuCaptureModeDrops_;
                continue;
            }
            std::uint32_t slotIndex = 0;
            if (!ClaimWritablePair(slotIndex)) {
                ++droppedFrames_;
                ++cpuCaptureSlotDrops_;
                // Keeping this completed frame at the head used to turn a
                // short host-side slot conflict into a growing latency
                // backlog. Drop it; OpenXR can reuse the prior complete pair.
                RetireCpuCaptureHead();
                return;
            }

            const std::uint64_t generation = ++generation_;
            for (std::uint32_t eye = 0;
                 eye < FEARVR_EYE_COUNT; ++eye) {
                FearVrSlot& slot = shared_->slot[eye][slotIndex];
                slot.frameId = frame.frameId;
                slot.generation = generation;
            }

            const auto transferStart =
                std::chrono::steady_clock::now();
            bool uploaded = true;
            if (frame.stereo) {
                for (std::uint32_t eye = 0;
                     eye < FEARVR_EYE_COUNT; ++eye) {
                    if (!StageSurfaceViaCpu(
                            device, frame.surface[eye].Get()) ||
                        !UploadCpuSurface(eye, slotIndex)) {
                        uploaded = false;
                        break;
                    }
                }
            } else {
                // A mono Present feeds both protocol eyes, but its pixels
                // only need to cross the classic-D3D9 readback boundary
                // once. Upload the same CPU image to both shared slots.
                uploaded = StageSurfaceViaCpu(
                    device,
                    frame.surface[FEARVR_EYE_LEFT].Get());
                for (std::uint32_t eye = 0;
                     uploaded && eye < FEARVR_EYE_COUNT; ++eye) {
                    uploaded = UploadCpuSurface(eye, slotIndex);
                }
            }
            const auto transferMicroseconds =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - transferStart)
                        .count());
            cpuCaptureTransferMaxMicroseconds_ = (std::max)(
                cpuCaptureTransferMaxMicroseconds_,
                transferMicroseconds);

            if (!uploaded) {
                ReleaseClaimedPair(slotIndex);
            } else {
                if (frame.stereo && !frame.flatPanel) {
                    InterlockedOr(
                        AtomicFlags(*shared_), FEARVR_BF_STEREO_ACTIVE);
                    ++stereoFrames_;
                } else {
                    InterlockedAnd(
                        AtomicFlags(*shared_),
                        static_cast<LONG>(
                            ~FEARVR_BF_STEREO_ACTIVE));
                }
                PendingFrame& pending = pending_[slotIndex];
                pending.active = true;
                pending.slotIndex = slotIndex;
                pending.frameId = frame.frameId;
                pending.generation = generation;
                ++cpuCaptureTransferred_;
            }

            RetireCpuCaptureHead();

            if (cpuCaptureTransferred_ != 0 &&
                cpuCaptureTransferred_ % 300 == 0) {
                std::ostringstream message;
                message << "queued=" << cpuCaptureQueued_
                        << " transferred=" << cpuCaptureTransferred_
                        << " queue_drops=" << cpuCaptureQueueDrops_
                        << " mode_drops=" << cpuCaptureModeDrops_
                        << " stale_drops=" << cpuCaptureStaleDrops_
                        << " slot_drops=" << cpuCaptureSlotDrops_
                        << " duplicate_drops="
                        << stereoDuplicateCaptureDrops_
                        << " query_not_ready="
                        << cpuCaptureQueryNotReady_
                        << " transfer_max_us="
                        << cpuCaptureTransferMaxMicroseconds_;
                logger_.Write(
                    "INFO", "cpu_capture_pipeline", message.str());
                cpuCaptureQueryNotReady_ = 0;
                cpuCaptureTransferMaxMicroseconds_ = 0;
            }
            return;
        }
    }

    bool CopyFrameViaCpu(IDirect3DDevice9* device,
                         IDirect3DSurface9* backBuffer,
                         std::uint32_t slotIndex) noexcept {
        HRESULT result = device->StretchRect(
            backBuffer, nullptr, gameCapture_.Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("cpu_bridge_stretch_failed", result);
            return false;
        }
        if (!StageSurfaceViaCpu(device, gameCapture_.Get())) {
            return false;
        }

        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            if (!UploadCpuSurface(eye, slotIndex)) {
                return false;
            }
        }
        return true;
    }

    bool CopyStereoFrameViaCpu(IDirect3DDevice9* device,
                               IDirect3DSurface9* backBuffer,
                               std::uint32_t slotIndex) noexcept {
        if (config_.stereoHudEnabled && hudCompositor_.ready()) {
            if (CopyStereoHudFrameViaGpu(device, backBuffer, slotIndex)) {
                return true;
            }
            // Compose() hat sich beim Fehlschlag selbst abgeschaltet; der
            // nächste Frame nimmt dann direkt den CPU-Weg.
        }
        if (config_.stereoHudEnabled &&
            rightWorldReadback_ && presentedReadback_) {
            return CopyStereoHudFrameViaCpu(
                device, backBuffer, slotIndex);
        }
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            if (!StageSurfaceViaCpu(
                    device, stereoCapture_[eye].Get()) ||
                !UploadCpuSurface(eye, slotIndex)) {
                return false;
            }
        }
        return true;
    }

    bool ReadSurfaceViaCpu(IDirect3DDevice9* device,
                           IDirect3DSurface9* source,
                           IDirect3DSurface9* destination,
                           const char* failureEvent) noexcept {
        const HRESULT result =
            device->GetRenderTargetData(source, destination);
        if (FAILED(result)) {
            LogHresult(failureEvent, result);
            return false;
        }
        return true;
    }

    // Derselbe Bildaufbau wie CopyStereoHudFrameViaCpu, nur entscheidet ein
    // Pixelshader statt einer Schleife. Übrig bleibt der Transfer-Readback,
    // den das klassische D3D9-Gerät des Spiels erzwingt.
    bool CopyStereoHudFrameViaGpu(IDirect3DDevice9* device,
                                  IDirect3DSurface9* backBuffer,
                                  std::uint32_t slotIndex) noexcept {
        HRESULT result = device->StretchRect(
            backBuffer, nullptr, gameCapture_.Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("stereo_hud_present_stretch_failed", result);
            return false;
        }

        const std::uint64_t totalPixels =
            static_cast<std::uint64_t>(width_) * height_;
        // Der Deckungsgrad stammt aus dem vorherigen Bild. Vor dem ersten
        // steht er auf 1.0, was als Vollbildeffekt gilt — das HUD wird also
        // erst ab dem zweiten Bild angehoben, nie versehentlich zu früh.
        const std::uint64_t changedPixels = static_cast<std::uint64_t>(
            hudCompositor_.coverageRatio() *
                static_cast<double>(totalPixels) + 0.5);
        const bool flatPanel = menuActive_;
        const bool composite =
            !flatPanel &&
            IsSafePostWorldCoverage(changedPixels, totalPixels);
        stereoHudFlatFrame_ = flatPanel;

        IDirect3DTexture9* const eyeWorld[FEARVR_EYE_COUNT] = {
            stereoCaptureTexture_[FEARVR_EYE_LEFT].Get(),
            stereoCaptureTexture_[FEARVR_EYE_RIGHT].Get()};
        if (!hudCompositor_.Compose(
                device, gameCaptureTexture_.Get(),
                stereoCaptureTexture_[FEARVR_EYE_RIGHT].Get(), eyeWorld,
                composite, flatPanel, logger_)) {
            return false;
        }

        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            if (!StageSurfaceViaCpu(
                    device, hudCompositor_.CompositeSurface(eye)) ||
                !UploadCpuSurface(eye, slotIndex)) {
                return false;
            }
        }

        ++stereoHudFrames_;
        if (stereoHudFrames_ == 1 || stereoHudFrames_ % 300 == 0) {
            std::ostringstream message;
            message << "changed_pixels=" << changedPixels
                    << " coverage_percent="
                    << (totalPixels == 0
                            ? 0
                            : changedPixels * 100u / totalPixels)
                    << " mode="
                    << (flatPanel
                            ? "flat_panel"
                            : (composite ? "raised_hud" : "world_only"))
                    << " path=gpu";
            logger_.Write(
                composite || flatPanel ? "INFO" : "WARN",
                flatPanel
                    ? "stereo_hud_flat_panel"
                    : (composite ? "stereo_hud_composited"
                                 : "stereo_hud_rejected"),
                message.str());
        }
        return true;
    }

    bool CopyStereoHudFrameViaCpu(IDirect3DDevice9* device,
                                  IDirect3DSurface9* backBuffer,
                                  std::uint32_t slotIndex) noexcept {
        HRESULT result = device->StretchRect(
            backBuffer, nullptr, gameCapture_.Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("stereo_hud_present_stretch_failed", result);
            return false;
        }
        if (!ReadSurfaceViaCpu(
                device, gameCapture_.Get(), presentedReadback_.Get(),
                "stereo_hud_present_readback_failed") ||
            !ReadSurfaceViaCpu(
                device, stereoCapture_[FEARVR_EYE_RIGHT].Get(),
                rightWorldReadback_.Get(),
                "stereo_hud_right_readback_failed")) {
            return false;
        }

        D3DLOCKED_RECT presented{};
        D3DLOCKED_RECT rightWorld{};
        result = presentedReadback_->LockRect(
            &presented, nullptr, D3DLOCK_READONLY);
        if (FAILED(result)) {
            LogHresult("stereo_hud_present_lock_failed", result);
            return false;
        }
        result = rightWorldReadback_->LockRect(
            &rightWorld, nullptr, D3DLOCK_READONLY);
        if (FAILED(result)) {
            presentedReadback_->UnlockRect();
            LogHresult("stereo_hud_right_lock_failed", result);
            return false;
        }

        std::uint64_t changedPixels = 0;
        for (UINT row = 0; row < height_; ++row) {
            const auto* presentedPixels =
                reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::uint8_t*>(presented.pBits) +
                    static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(presented.Pitch));
            const auto* rightPixels =
                reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::uint8_t*>(rightWorld.pBits) +
                    static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(rightWorld.Pitch));
            for (UINT column = 0; column < width_; ++column) {
                changedPixels += IsPostWorldPixel(
                    presentedPixels[column], rightPixels[column])
                    ? 1u
                    : 0u;
            }
        }
        const std::uint64_t totalPixels =
            static_cast<std::uint64_t>(width_) * height_;
        // Menu state is supplied by the verified Retail menu/freshness hooks.
        // Pixel coverage alone cannot distinguish a menu from a fullscreen
        // gameplay effect such as Slow-Mo, so it must not select mono video
        // mode anymore.
        const bool flatPanel = menuActive_;
        const bool composite =
            !flatPanel && IsSafePostWorldCoverage(changedPixels, totalPixels);
        stereoHudFlatFrame_ = flatPanel;

        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            D3DLOCKED_RECT source{};
            bool sourceLocked = false;
            if (flatPanel) {
                source = presented;
            } else if (eye == FEARVR_EYE_RIGHT) {
                source = rightWorld;
            } else {
                if (!ReadSurfaceViaCpu(
                        device, stereoCapture_[eye].Get(),
                        gameReadback_.Get(),
                        "stereo_hud_left_readback_failed")) {
                    rightWorldReadback_->UnlockRect();
                    presentedReadback_->UnlockRect();
                    return false;
                }
                result = gameReadback_->LockRect(
                    &source, nullptr, D3DLOCK_READONLY);
                if (FAILED(result)) {
                    rightWorldReadback_->UnlockRect();
                    presentedReadback_->UnlockRect();
                    LogHresult("stereo_hud_left_lock_failed", result);
                    return false;
                }
                sourceLocked = true;
            }

            D3DLOCKED_RECT destination{};
            result = bridgeUpload_->LockRect(
                &destination, nullptr, 0);
            if (FAILED(result)) {
                if (sourceLocked) {
                    gameReadback_->UnlockRect();
                }
                rightWorldReadback_->UnlockRect();
                presentedReadback_->UnlockRect();
                LogHresult("stereo_hud_upload_lock_failed", result);
                return false;
            }

            for (UINT row = 0; row < height_; ++row) {
                const UINT hudSourceRow =
                    StereoHudSourceRow(row, height_);
                const auto* sourcePixels =
                    reinterpret_cast<const std::uint32_t*>(
                        static_cast<const std::uint8_t*>(source.pBits) +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(source.Pitch));
                const auto* presentedPixels =
                    reinterpret_cast<const std::uint32_t*>(
                        static_cast<const std::uint8_t*>(presented.pBits) +
                        static_cast<std::size_t>(
                            hudSourceRow < height_ ? hudSourceRow : row) *
                            static_cast<std::size_t>(presented.Pitch));
                const auto* rightPixels =
                    reinterpret_cast<const std::uint32_t*>(
                        static_cast<const std::uint8_t*>(rightWorld.pBits) +
                        static_cast<std::size_t>(
                            hudSourceRow < height_ ? hudSourceRow : row) *
                            static_cast<std::size_t>(rightWorld.Pitch));
                auto* destinationPixels =
                    reinterpret_cast<std::uint32_t*>(
                        static_cast<std::uint8_t*>(destination.pBits) +
                        static_cast<std::size_t>(row) *
                            static_cast<std::size_t>(destination.Pitch));
                for (UINT column = 0; column < width_; ++column) {
                    const UINT hudSourceColumn =
                        StereoHudSourceColumn(
                            column, row, width_, height_);
                    const bool overlayPixel =
                        composite && hudSourceRow < height_ &&
                        hudSourceColumn < width_ &&
                        IsPostWorldPixel(
                            presentedPixels[hudSourceColumn],
                            rightPixels[hudSourceColumn]);
                    destinationPixels[column] = flatPanel
                        ? sourcePixels[column]
                        : (overlayPixel
                               ? presentedPixels[hudSourceColumn]
                                        : sourcePixels[column]);
                }
            }
            bridgeUpload_->UnlockRect();
            if (sourceLocked) {
                gameReadback_->UnlockRect();
            }
            if (!UploadCpuSurface(eye, slotIndex)) {
                rightWorldReadback_->UnlockRect();
                presentedReadback_->UnlockRect();
                return false;
            }
        }
        rightWorldReadback_->UnlockRect();
        presentedReadback_->UnlockRect();

        ++stereoHudFrames_;
        if (stereoHudFrames_ == 1 ||
            stereoHudFrames_ % 300 == 0) {
            std::ostringstream message;
            message << "changed_pixels=" << changedPixels
                    << " coverage_percent="
                    << (totalPixels == 0
                            ? 0
                            : changedPixels * 100u / totalPixels)
                    << " mode="
                    << (flatPanel
                            ? "flat_panel"
                            : (composite ? "raised_hud"
                                         : "world_only"));
            logger_.Write(
                composite || flatPanel ? "INFO" : "WARN",
                flatPanel
                    ? "stereo_hud_flat_panel"
                    : (composite ? "stereo_hud_composited"
                                 : "stereo_hud_rejected"),
                message.str());
        }
        return true;
    }

    void ReleaseResources() noexcept {
        RestoreStereoRenderTarget();
        pending_ = {};
        resourcesReady_ = false;
        transferMode_ = TransferMode::None;
        // Zuerst der Kompositor: Er hält Render-Targets auf demselben Gerät.
        hudCompositor_.Release();
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            stereoRenderTarget_[eye].Reset();
            stereoCaptureTexture_[eye].Reset();
            stereoCapture_[eye].Reset();
            for (std::uint32_t slotIndex = 0;
                 slotIndex < FEARVR_SLOTS_PER_EYE; ++slotIndex) {
                SlotResource& resource = resources_[eye][slotIndex];
                resource.completion.Reset();
                resource.surface.Reset();
                resource.texture.Reset();
                resource.sharedHandle = nullptr;
                if (shared_ != nullptr) {
                    FearVrSlot& slot = shared_->slot[eye][slotIndex];
                    slot.sharedHandle = 0;
                    slot.frameId = 0;
                    slot.width = 0;
                    slot.height = 0;
                    slot.format = FEARVR_FMT_UNKNOWN;
                    slot.generation = 0;
                    InterlockedExchange(AtomicState(slot),
                                        FEARVR_SLOT_EMPTY);
                }
            }
        }
        stereoDepthStencil_.Reset();
        stereoSupersamplingReady_ = false;
        stereoViewportScaleLogged_ = false;
        stereoScissorScaleLogged_ = false;
        bridgeUpload_.Reset();
        presentedReadback_.Reset();
        rightWorldReadback_.Reset();
        gameReadback_.Reset();
        for (CpuCaptureFrame& frame : cpuCaptureQueue_) {
            frame.active = false;
            frame.completion.Reset();
            for (std::uint32_t eye = 0;
                 eye < FEARVR_EYE_COUNT; ++eye) {
                frame.surface[eye].Reset();
                frame.texture[eye].Reset();
            }
        }
        cpuCaptureRead_ = 0;
        cpuCaptureWrite_ = 0;
        cpuCaptureCount_ = 0;
        gameCapture_.Reset();
        gameCaptureTexture_.Reset();
        ClearStereoFrame();
        bridgeDeviceEx_.Reset();
        bridgeDirect3DEx_.Reset();
        if (companionWindow_ != nullptr) {
            DestroyWindow(companionWindow_);
            companionWindow_ = nullptr;
        }
        if (shared_ != nullptr) {
            InterlockedAnd(
                AtomicFlags(*shared_),
                static_cast<LONG>(
                    ~(FEARVR_BF_SHARED_SUPPORTED |
                      FEARVR_BF_CPU_FALLBACK |
                      FEARVR_BF_STEREO_ACTIVE)));
        }
    }

    bool ClaimWritablePair(std::uint32_t& slotIndex) noexcept {
        for (std::uint32_t attempt = 0;
             attempt < FEARVR_SLOTS_PER_EYE; ++attempt) {
            const std::uint32_t candidate =
                (nextSlot_ + attempt) % FEARVR_SLOTS_PER_EYE;
            FearVrSlot& left =
                shared_->slot[FEARVR_EYE_LEFT][candidate];
            FearVrSlot& right =
                shared_->slot[FEARVR_EYE_RIGHT][candidate];
            if (InterlockedCompareExchange(
                    AtomicState(left), FEARVR_SLOT_WRITING,
                    FEARVR_SLOT_EMPTY) != FEARVR_SLOT_EMPTY) {
                continue;
            }
            if (InterlockedCompareExchange(
                    AtomicState(right), FEARVR_SLOT_WRITING,
                    FEARVR_SLOT_EMPTY) != FEARVR_SLOT_EMPTY) {
                InterlockedExchange(AtomicState(left),
                                    FEARVR_SLOT_EMPTY);
                continue;
            }
            nextSlot_ = (candidate + 1) % FEARVR_SLOTS_PER_EYE;
            slotIndex = candidate;
            return true;
        }
        return false;
    }

    void ReleaseClaimedPair(std::uint32_t slotIndex) noexcept {
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            InterlockedExchange(
                AtomicState(shared_->slot[eye][slotIndex]),
                FEARVR_SLOT_EMPTY);
        }
    }

    void PollPending() noexcept {
        for (PendingFrame& pending : pending_) {
            if (!pending.active) {
                continue;
            }
            bool complete = true;
            for (std::uint32_t eye = 0;
                 eye < FEARVR_EYE_COUNT; ++eye) {
                SlotResource& resource =
                    resources_[eye][pending.slotIndex];
                if (!resource.completion ||
                    resource.completion->GetData(
                        nullptr, 0, D3DGETDATA_FLUSH) != S_OK) {
                    complete = false;
                    break;
                }
            }
            if (!complete) {
                continue;
            }

            MemoryBarrier();
            for (std::uint32_t eye = 0;
                 eye < FEARVR_EYE_COUNT; ++eye) {
                InterlockedExchange(
                    AtomicState(
                        shared_->slot[eye][pending.slotIndex]),
                    FEARVR_SLOT_READY);
            }
            SetEvent(frameReadyEvent_);
            if (pending.frameId == 1 ||
                pending.frameId % 300 == 0) {
                std::ostringstream message;
                message << "frame=" << pending.frameId
                        << " generation=" << pending.generation
                        << " slot=" << pending.slotIndex;
                logger_.Write(
                    "INFO", "frame_ready", message.str());
            }
            pending = {};
        }
    }

    void LogHresult(const char* event, HRESULT result) noexcept {
        std::ostringstream message;
        message << "HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        logger_.Write("ERROR", event, message.str());
    }

    void LogWin32(const char* event, DWORD error) noexcept {
        logger_.Write("ERROR", event,
                      "Win32=" + std::to_string(error));
    }

    CommandLineConfig config_;
    Logger logger_;
    std::mutex mutex_;
    HANDLE mapping_{nullptr};
    HANDLE frameReadyEvent_{nullptr};
    HANDLE slotConsumedEvent_{nullptr};
    HANDLE renderRequestEvent_{nullptr};
    FearVrSharedHeader* shared_{nullptr};
    IDirect3DDevice9* device_{nullptr};
    ComPtr<IDirect3D9Ex> bridgeDirect3DEx_;
    ComPtr<IDirect3DDevice9Ex> bridgeDeviceEx_;
    ComPtr<IDirect3DTexture9> gameCaptureTexture_;
    ComPtr<IDirect3DSurface9> gameCapture_;
    ComPtr<IDirect3DSurface9> gameReadback_;
    ComPtr<IDirect3DSurface9> rightWorldReadback_;
    ComPtr<IDirect3DSurface9> presentedReadback_;
    ComPtr<IDirect3DSurface9> bridgeUpload_;
    std::array<ComPtr<IDirect3DTexture9>, FEARVR_EYE_COUNT>
        stereoCaptureTexture_{};
    std::array<ComPtr<IDirect3DSurface9>, FEARVR_EYE_COUNT>
        stereoCapture_{};
    std::array<ComPtr<IDirect3DSurface9>, FEARVR_EYE_COUNT>
        stereoRenderTarget_{};
    ComPtr<IDirect3DSurface9> stereoDepthStencil_;
    ComPtr<IDirect3DSurface9> savedRenderTarget_;
    ComPtr<IDirect3DSurface9> savedDepthStencil_;
    D3DVIEWPORT9 savedViewport_{};
    RECT savedScissorRect_{};
    GpuHudCompositor hudCompositor_;
    std::array<std::array<SlotResource, FEARVR_SLOTS_PER_EYE>,
               FEARVR_EYE_COUNT>
        resources_{};
    std::array<CpuCaptureFrame, kCpuCaptureQueueSize>
        cpuCaptureQueue_{};
    std::array<PendingFrame, FEARVR_SLOTS_PER_EYE> pending_{};
    UINT sourceWidth_{0};
    UINT sourceHeight_{0};
    UINT width_{0};
    UINT height_{0};
    std::uint32_t effectiveRenderScalePercent_{
        kRenderScaleMinimumPercent};
    std::uint32_t nextSlot_{0};
    std::uint64_t frameId_{0};
    std::uint64_t generation_{0};
    std::uint64_t droppedFrames_{0};
    std::uint64_t stereoFrameId_{0};
    std::uint64_t lastStagedStereoFrameId_{0};
    std::uint64_t stereoFrames_{0};
    std::uint64_t stereoHudFrames_{0};
    std::uint64_t stereoDuplicateCaptureDrops_{0};
    std::uint64_t cpuCaptureQueued_{0};
    std::uint64_t cpuCaptureTransferred_{0};
    std::uint64_t cpuCaptureQueueDrops_{0};
    std::uint64_t cpuCaptureModeDrops_{0};
    std::uint64_t cpuCaptureStaleDrops_{0};
    std::uint64_t cpuCaptureSlotDrops_{0};
    std::uint64_t cpuCaptureQueryNotReady_{0};
    std::uint64_t cpuCaptureTransferMaxMicroseconds_{0};
    std::uint64_t xrPacingWaits_{0};
    std::uint64_t xrPacingTimeouts_{0};
    std::uint64_t xrPacingWaitMaxMilliseconds_{0};
    std::uint64_t bypassPresentCount_{0};
    std::chrono::steady_clock::time_point bypassPresentWindowStart_{};
    std::uint64_t gameAdapterLuid_{0};
    std::uint64_t lastHostHeartbeat_{0};
    ULONGLONG lastHostHeartbeatTick_{0};
    ULONGLONG nextResourceRetryTick_{0};
    HWND companionWindow_{nullptr};
    std::size_t cpuCaptureRead_{0};
    std::size_t cpuCaptureWrite_{0};
    std::size_t cpuCaptureCount_{0};
    TransferMode transferMode_{TransferMode::None};
    bool resourcesReady_{false};
    bool deviceMetadataReady_{false};
    bool hostConnected_{false};
    bool adapterMatchLogged_{false};
    bool adapterMismatchLogged_{false};
    std::array<bool, FEARVR_EYE_COUNT> stereoEyeCaptured_{};
    bool stereoFrameReady_{false};
    bool stereoHudFlatFrame_{false};
    bool stereoAccepting_{false};
    bool stereoIncompleteLogged_{false};
    bool stereoSupersamplingReady_{false};
    bool stereoRenderTargetActive_{false};
    bool savedViewportValid_{false};
    bool savedScissorValid_{false};
    bool stereoViewportScaleLogged_{false};
    bool stereoScissorScaleLogged_{false};
    std::uint32_t stereoRenderTargetEye_{FEARVR_EYE_LEFT};
    bool xrFramePacingLogged_{false};
    bool stereoKeyWasDown_{false};
    bool recenterKeyWasDown_{false};
    bool comfortKeyWasDown_{false};
    bool comfortModeEnabled_{false};
    bool menuActive_{false};
    std::uint32_t renderModeGeneration_{1};
    std::uint32_t recenterGeneration_{0};
    std::uint32_t fovScalePercent_{
        FEARVR_FOV_SCALE_DEFAULT_PERCENT};
    StereoToggleCallback stereoToggleCallback_{nullptr};
};

Bridge& GetBridge() {
    // Intentionally process-lifetime: CRT destructors run under the loader
    // lock during DLL_DETACH, where D3D and IPC cleanup is unsafe. Windows
    // releases the process resources when the process exits.
    static Bridge* bridge = new Bridge;
    return *bridge;
}

using CreateDeviceFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceExFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using CreateAdditionalSwapChainFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using GetSwapChainFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using DeviceAddRefFunction = ULONG(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*);
using DeviceReleaseFunction = ULONG(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*);
using CreateTextureFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DTexture9**, HANDLE*);
using CreateVolumeTextureFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
    D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
using CreateCubeTextureFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DCubeTexture9**, HANDLE*);
using CreateVertexBufferFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
    IDirect3DVertexBuffer9**, HANDLE*);
using CreateIndexBufferFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DIndexBuffer9**, HANDLE*);
using ResetFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,
                                D3DPRESENT_PARAMETERS*);
using PresentFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*,
                                const RECT*, HWND, const RGNDATA*);
using SwapChainPresentFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
    const RGNDATA*, DWORD);
using ResetExFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using PresentExFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
    const RGNDATA*, DWORD);
using SetIndicesFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DIndexBuffer9*);
using GetIndicesFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DIndexBuffer9**);
using SetViewportFunction =
    HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetScissorRectFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);

void ForceHighQualitySource(D3DPRESENT_PARAMETERS* parameters) noexcept {
    // Keep Retail's own display mode. The game menu relies on that mode's
    // coordinate system; overriding it breaks the flat VR menu projection.
    (void)parameters;
}

struct D3D9VtableRecord {
    void** vtable{nullptr};
    CreateDeviceFunction createDevice{nullptr};
    CreateDeviceExFunction createDeviceEx{nullptr};
    bool createDeviceHookActive{false};
    bool createDeviceExHookActive{false};
};

struct DeviceVtableRecord {
    void** vtable{nullptr};
    DeviceAddRefFunction addRef{nullptr};
    DeviceReleaseFunction release{nullptr};
    CreateAdditionalSwapChainFunction createAdditionalSwapChain{nullptr};
    GetSwapChainFunction getSwapChain{nullptr};
    CreateTextureFunction createTexture{nullptr};
    CreateVolumeTextureFunction createVolumeTexture{nullptr};
    CreateCubeTextureFunction createCubeTexture{nullptr};
    CreateVertexBufferFunction createVertexBuffer{nullptr};
    CreateIndexBufferFunction createIndexBuffer{nullptr};
    ResetFunction reset{nullptr};
    PresentFunction present{nullptr};
    ResetExFunction resetEx{nullptr};
    PresentExFunction presentEx{nullptr};
    SetViewportFunction setViewport{nullptr};
    SetScissorRectFunction setScissorRect{nullptr};
    bool baseHooksActive{false};
    bool managedResourceHooksActive{false};
    bool exHooksActive{false};
};

struct SwapChainVtableRecord {
    void** vtable{nullptr};
    SwapChainPresentFunction present{nullptr};
};

SRWLOCK g_hookLock = SRWLOCK_INIT;
SRWLOCK g_appLocalHookLock = SRWLOCK_INIT;
volatile LONG g_appLocalHooksActive = FALSE;
volatile LONG g_appLocalHooksPoisoned = FALSE;
std::array<D3D9VtableRecord, 8> g_d3d9Records{};
std::array<DeviceVtableRecord, 8> g_deviceRecords{};
std::array<SwapChainVtableRecord, 8> g_swapChainRecords{};
INIT_ONCE g_lateHookOnce = INIT_ONCE_STATIC_INIT;
volatile LONG g_lateHooksActive = FALSE;
BOOL g_lateHookResult = FALSE;
ResetFunction g_lateReset = nullptr;
PresentFunction g_latePresent = nullptr;
ResetFunction g_appLocalReset = nullptr;
PresentFunction g_appLocalPresent = nullptr;
SetIndicesFunction g_appLocalSetIndices = nullptr;
GetIndicesFunction g_appLocalGetIndices = nullptr;
void* g_appLocalResetTarget = nullptr;
void* g_appLocalPresentTarget = nullptr;
void* g_appLocalSetIndicesTarget = nullptr;
void* g_appLocalGetIndicesTarget = nullptr;
SetViewportFunction g_lateSetViewport = nullptr;
SetScissorRectFunction g_lateSetScissorRect = nullptr;
volatile LONG g_managedResourceTranslationLogged = FALSE;
volatile LONG g_managedIndexCreateDetailsLogged = FALSE;
volatile LONG g_managedIndexLockLogged = FALSE;
volatile LONG g_swapChainHookLogged = FALSE;
alignas(8) volatile LONG64 g_presentHookCounts[3]{};
LONG g_managedResourceSuccessLogged[5]{};
bool TranslateManagedResource(
    DWORD& usage, D3DPOOL& pool,
    bool requiresDynamicLocking) noexcept {
    if (pool != D3DPOOL_MANAGED) {
        return false;
    }
    pool = D3DPOOL_DEFAULT;
    if (requiresDynamicLocking) {
        usage |= D3DUSAGE_DYNAMIC;
    }
    if (InterlockedCompareExchange(
            &g_managedResourceTranslationLogged, TRUE, FALSE) == FALSE) {
        GetBridge().LogHookStatus(
            "INFO", "d3d9ex_managed_translation",
            "D3DPOOL_MANAGED resources use persistent D3D9Ex "
            "allocations; textures remain dynamically lockable while "
            "static vertex/index buffers retain their original usage.");
    }
    return true;
}

// D3D9Ex rejects D3DPOOL_MANAGED, but Jupiter EX queries the descriptor of
// its persistent UI index buffer and relies on the pool identity it requested.
// Keep the real allocation in D3DPOOL_DEFAULT while presenting the classic
// descriptor to the game. The device hook unwraps this private interface
// before forwarding SetIndices to the D3D9Ex runtime.
constexpr GUID kManagedIndexBufferInner = {
    0xc102956e, 0xe31a, 0x4adc,
    {0x89, 0xbb, 0x4f, 0x7d, 0x59, 0xe4, 0x63, 0x2a}};

class ManagedIndexBufferCompat;
SRWLOCK g_managedIndexRegistryLock = SRWLOCK_INIT;
constexpr std::size_t kManagedIndexBufferCapacity = 4096;
std::array<ManagedIndexBufferCompat*, kManagedIndexBufferCapacity>
    g_managedIndexBuffers{};
struct BoundManagedIndexBuffer {
    IDirect3DDevice9* device{nullptr};
    ManagedIndexBufferCompat* buffer{nullptr};
};
SRWLOCK g_boundManagedIndexLock = SRWLOCK_INIT;
std::recursive_mutex g_managedIndexBindingCallMutex;
std::array<BoundManagedIndexBuffer, 64>
    g_boundManagedIndexBuffers{};
volatile LONG g_nextManagedIndexDebugId = 0;
volatile LONG g_managedIndexLifecycleSequence = 0;

class ManagedIndexBufferCompat final : public IDirect3DIndexBuffer9 {
public:
    ManagedIndexBufferCompat(
        IDirect3DIndexBuffer9* inner,
        DWORD requestedUsage, D3DFORMAT format,
        UINT length)
        : inner_(inner),
          debugId_(InterlockedIncrement(
              &g_nextManagedIndexDebugId)) {
        description_.Format = format;
        description_.Type = D3DRTYPE_INDEXBUFFER;
        description_.Usage = requestedUsage;
        description_.Pool = D3DPOOL_MANAGED;
        description_.Size = length;
        shadow_.resize(length);
        bool registered = false;
        AcquireSRWLockExclusive(&g_managedIndexRegistryLock);
        for (auto& buffer : g_managedIndexBuffers) {
            if (buffer == nullptr) {
                buffer = this;
                registered = true;
                registered_ = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_managedIndexRegistryLock);
        std::ostringstream message;
        message << "sequence="
                << InterlockedIncrement(
                       &g_managedIndexLifecycleSequence)
                << " id=" << debugId_
                << " wrapper=0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(this)
                << " inner=0x"
                << reinterpret_cast<std::uintptr_t>(inner_)
                << std::dec << " length=" << length
                << " registered=" << (registered ? 1 : 0);
        GetBridge().LogHookStatus(
            registered ? "INFO" : "ERROR",
            "d3d9ex_managed_index_lifecycle_create",
            message.str());
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interfaceId, void** output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (interfaceId == __uuidof(IUnknown) ||
            interfaceId == __uuidof(IDirect3DResource9) ||
            interfaceId == __uuidof(IDirect3DIndexBuffer9)) {
            *output = static_cast<IDirect3DIndexBuffer9*>(this);
            AddRef();
            return S_OK;
        }
        if (interfaceId == kManagedIndexBufferInner) {
            *output = inner_;
            inner_->AddRef();
            const LONG ordinal = InterlockedIncrement(
                &innerQueryCount_);
            if (ordinal <= 8) {
                std::ostringstream message;
                message << "sequence="
                        << InterlockedIncrement(
                               &g_managedIndexLifecycleSequence)
                        << " id=" << debugId_
                        << " ordinal=" << ordinal;
                GetBridge().LogHookStatus(
                    "INFO",
                    "d3d9ex_managed_index_lifecycle_unwrap",
                    message.str());
            }
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddReference("add_ref");
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseReference("release");
    }

    ULONG RetainForSnapshot() noexcept {
        return AddReference("snapshot_add_ref");
    }

    ULONG ReleaseFromSnapshot() noexcept {
        return ReleaseReference("snapshot_release");
    }

    ULONG RetainForBinding() noexcept {
        return AddReference("binding_add_ref");
    }

    ULONG ReleaseFromBinding() noexcept {
        return ReleaseReference("binding_release");
    }

    IDirect3DIndexBuffer9* Inner() const noexcept {
        return inner_;
    }

    bool Registered() const noexcept {
        return registered_;
    }

    void QuarantineAfterDeviceLoss() noexcept {
        AcquireSRWLockExclusive(&g_managedIndexRegistryLock);
        for (auto& buffer : g_managedIndexBuffers) {
            if (buffer == this) {
                buffer = nullptr;
                break;
            }
        }
        registered_ = false;
        ReleaseSRWLockExclusive(&g_managedIndexRegistryLock);
        LogLifecycleState("quarantined_after_device_loss", E_UNEXPECTED);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(
        IDirect3DDevice9** device) override {
        return inner_->GetDevice(device);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID guid, const void* data, DWORD size,
        DWORD flags) override {
        return inner_->SetPrivateData(guid, data, size, flags);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID guid, void* data, DWORD* size) override {
        return inner_->GetPrivateData(guid, data, size);
    }

    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        return inner_->FreePrivateData(guid);
    }

    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override {
        return inner_->SetPriority(priority);
    }

    DWORD STDMETHODCALLTYPE GetPriority() override {
        return inner_->GetPriority();
    }

    void STDMETHODCALLTYPE PreLoad() override {
        inner_->PreLoad();
    }

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
        return D3DRTYPE_INDEXBUFFER;
    }

    HRESULT STDMETHODCALLTYPE Lock(
        UINT offset, UINT size, void** data, DWORD flags) override {
        // If the application never delivered the Unlock that belonged to a
        // pre-reset Lock, do not let the stale suppression consume a future,
        // unrelated Unlock.
        suppressNextUnlock_ = false;
        const HRESULT result =
            inner_->Lock(offset, size, data, flags);
        const bool dataReturned =
            SUCCEEDED(result) && data != nullptr && *data != nullptr;
        if (dataReturned && offset <= shadow_.size()) {
            lockedOffset_ = offset;
            lockedSize_ = size != 0
                ? (std::min)(
                    static_cast<std::size_t>(size),
                    shadow_.size() - offset)
                : shadow_.size() - offset;
            lockedData_ = *data;
            if (InterlockedCompareExchange(
                    &g_managedIndexLockLogged,
                    TRUE, FALSE) == FALSE) {
                std::ostringstream message;
                message << "offset=" << offset
                        << " size=" << size
                        << " flags=0x" << std::hex << flags
                        << std::dec << " data=1";
                GetBridge().LogHookStatus(
                    "INFO", "d3d9ex_managed_index_lock",
                    message.str());
            }
        } else if (SUCCEEDED(result)) {
            lockedData_ = nullptr;
            lockedOffset_ = 0;
            lockedSize_ = 0;
        }
        const LONG ordinal = InterlockedIncrement(&lockLogCount_);
        if (ordinal <= 8 || FAILED(result)) {
            std::ostringstream message;
            message << "sequence="
                    << InterlockedIncrement(
                           &g_managedIndexLifecycleSequence)
                    << " id=" << debugId_
                    << " ordinal=" << ordinal
                    << " offset=" << offset
                    << " size=" << size
                    << " flags=0x" << std::hex << flags
                    << " HRESULT=0x"
                    << static_cast<std::uint32_t>(result)
                    << std::dec << " data="
                    << (dataReturned ? 1 : 0)
                    << " tracked_lock="
                    << (lockedData_ != nullptr ? 1 : 0);
            GetBridge().LogHookStatus(
                SUCCEEDED(result) ? "INFO" : "ERROR",
                "d3d9ex_managed_index_lifecycle_lock",
                message.str());
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Unlock() override {
        if (suppressNextUnlock_) {
            suppressNextUnlock_ = false;
            LogUnlockLifecycle(D3D_OK, true);
            return D3D_OK;
        }
        if (lockedData_ != nullptr && lockedSize_ != 0) {
            std::memcpy(
                shadow_.data() + lockedOffset_,
                lockedData_, lockedSize_);
            shadowReady_ = true;
        }
        lockedData_ = nullptr;
        lockedOffset_ = 0;
        lockedSize_ = 0;
        const HRESULT result = inner_->Unlock();
        LogUnlockLifecycle(result, false);
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(
        D3DINDEXBUFFER_DESC* description) override {
        if (description == nullptr) {
            return D3DERR_INVALIDCALL;
        }
        *description = description_;
        return D3D_OK;
    }

    HRESULT RestoreAfterReset() noexcept {
        if (!shadowReady_ || shadow_.empty()) {
            return S_FALSE;
        }
        void* destination = nullptr;
        HRESULT result = inner_->Lock(
            0, static_cast<UINT>(shadow_.size()),
            &destination, 0);
        if (FAILED(result) || destination == nullptr) {
            return FAILED(result) ? result : E_POINTER;
        }
        std::memcpy(
            destination, shadow_.data(), shadow_.size());
        result = inner_->Unlock();
        return result;
    }

    HRESULT PrepareForReset() noexcept {
        if (lockedData_ == nullptr) {
            return S_FALSE;
        }
        if (lockedSize_ != 0) {
            std::memcpy(
                shadow_.data() + lockedOffset_,
                lockedData_, lockedSize_);
            shadowReady_ = true;
        }
        lockedData_ = nullptr;
        lockedOffset_ = 0;
        lockedSize_ = 0;
        const HRESULT result = inner_->Unlock();
        if (SUCCEEDED(result)) {
            suppressNextUnlock_ = true;
        }
        return result;
    }

    LONG DebugId() const noexcept {
        return debugId_;
    }

    bool IsLocked() const noexcept {
        return lockedData_ != nullptr;
    }

    bool HasShadow() const noexcept {
        return shadowReady_;
    }

    void LogSetIndicesLifecycle(
        HRESULT result, bool unwrapped) noexcept {
        const LONG ordinal = InterlockedIncrement(&setIndicesCount_);
        if (ordinal > 8 && SUCCEEDED(result)) {
            return;
        }
        std::ostringstream message;
        message << "sequence="
                << InterlockedIncrement(
                       &g_managedIndexLifecycleSequence)
                << " id=" << debugId_
                << " ordinal=" << ordinal
                << " unwrapped=" << (unwrapped ? 1 : 0)
                << " HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        GetBridge().LogHookStatus(
            SUCCEEDED(result) && unwrapped ? "INFO" : "ERROR",
            "d3d9ex_managed_index_lifecycle_set_indices",
            message.str());
    }

    void LogResetSnapshot(
        const char* phase, HRESULT result) noexcept {
        LogLifecycleState(phase, result);
    }

private:
    ULONG AddReference(const char* operation) noexcept {
        const LONG remaining = InterlockedIncrement(&references_);
        const LONG ordinal = InterlockedIncrement(&addRefCount_);
        LogReferenceLifecycle(
            operation, ordinal, remaining, false);
        return static_cast<ULONG>(remaining);
    }

    ULONG ReleaseReference(const char* operation) noexcept {
        AcquireSRWLockExclusive(&g_managedIndexRegistryLock);
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) {
            for (auto& buffer : g_managedIndexBuffers) {
                if (buffer == this) {
                    buffer = nullptr;
                    break;
                }
            }
        }
        ReleaseSRWLockExclusive(&g_managedIndexRegistryLock);
        const LONG ordinal = InterlockedIncrement(&releaseCount_);
        LogReferenceLifecycle(
            operation, ordinal, remaining, remaining == 0);
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    void LogReferenceLifecycle(
        const char* operation, LONG ordinal, LONG remaining,
        bool force) noexcept {
        if (!force && ordinal > 16) {
            return;
        }
        std::ostringstream message;
        message << "sequence="
                << InterlockedIncrement(
                       &g_managedIndexLifecycleSequence)
                << " id=" << debugId_
                << " operation=" << operation
                << " ordinal=" << ordinal
                << " references=" << remaining;
        GetBridge().LogHookStatus(
            remaining >= 0 ? "INFO" : "ERROR",
            "d3d9ex_managed_index_lifecycle_reference",
            message.str());
    }

    void LogLifecycleState(
        const char* phase, HRESULT result) noexcept {
        std::ostringstream message;
        message << "sequence="
                << InterlockedIncrement(
                       &g_managedIndexLifecycleSequence)
                << " id=" << debugId_
                << " phase=" << phase
                << " references="
                << InterlockedCompareExchange(&references_, 0, 0)
                << " locks="
                << InterlockedCompareExchange(&lockLogCount_, 0, 0)
                << " unlocks="
                << InterlockedCompareExchange(&unlockLogCount_, 0, 0)
                << " set_indices="
                << InterlockedCompareExchange(&setIndicesCount_, 0, 0)
                << " unwraps="
                << InterlockedCompareExchange(&innerQueryCount_, 0, 0)
                << " add_refs="
                << InterlockedCompareExchange(&addRefCount_, 0, 0)
                << " releases="
                << InterlockedCompareExchange(&releaseCount_, 0, 0)
                << " locked=" << (lockedData_ != nullptr ? 1 : 0)
                << " shadow_ready=" << (shadowReady_ ? 1 : 0)
                << " suppress_unlock="
                << (suppressNextUnlock_ ? 1 : 0)
                << " HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        GetBridge().LogHookStatus(
            FAILED(result) ? "ERROR" : "INFO",
            "d3d9ex_managed_index_lifecycle_state",
            message.str());
    }

    void LogUnlockLifecycle(
        HRESULT result, bool suppressed) noexcept {
        const LONG ordinal = InterlockedIncrement(&unlockLogCount_);
        if (ordinal > 8 && SUCCEEDED(result)) {
            return;
        }
        std::ostringstream message;
        message << "sequence="
                << InterlockedIncrement(
                       &g_managedIndexLifecycleSequence)
                << " id=" << debugId_
                << " ordinal=" << ordinal
                << " HRESULT=0x" << std::hex
                << static_cast<std::uint32_t>(result)
                << std::dec << " shadow_ready="
                << (shadowReady_ ? 1 : 0)
                << " suppressed=" << (suppressed ? 1 : 0);
        GetBridge().LogHookStatus(
            SUCCEEDED(result) ? "INFO" : "ERROR",
            "d3d9ex_managed_index_lifecycle_unlock",
            message.str());
    }

    ~ManagedIndexBufferCompat() {
        LogLifecycleState("destroy", S_OK);
        inner_->Release();
    }

    volatile LONG references_{1};
    IDirect3DIndexBuffer9* inner_{nullptr};
    LONG debugId_{0};
    D3DINDEXBUFFER_DESC description_{};
    std::vector<std::uint8_t> shadow_;
    void* lockedData_{nullptr};
    std::size_t lockedOffset_{0};
    std::size_t lockedSize_{0};
    bool shadowReady_{false};
    bool suppressNextUnlock_{false};
    bool registered_{false};
    volatile LONG lockLogCount_{0};
    volatile LONG unlockLogCount_{0};
    volatile LONG setIndicesCount_{0};
    volatile LONG innerQueryCount_{0};
    volatile LONG addRefCount_{0};
    volatile LONG releaseCount_{0};
};

ManagedIndexBufferCompat* RetainManagedIndexBufferForBinding(
    IDirect3DIndexBuffer9* indexBuffer) noexcept {
    if (indexBuffer == nullptr) {
        return nullptr;
    }
    ManagedIndexBufferCompat* result = nullptr;
    AcquireSRWLockShared(&g_managedIndexRegistryLock);
    for (ManagedIndexBufferCompat* buffer : g_managedIndexBuffers) {
        if (static_cast<IDirect3DIndexBuffer9*>(buffer) == indexBuffer) {
            buffer->RetainForBinding();
            result = buffer;
            break;
        }
    }
    ReleaseSRWLockShared(&g_managedIndexRegistryLock);
    return result;
}

ManagedIndexBufferCompat* RetainManagedIndexBufferByInner(
    IDirect3DIndexBuffer9* inner) noexcept {
    if (inner == nullptr) {
        return nullptr;
    }
    ManagedIndexBufferCompat* result = nullptr;
    AcquireSRWLockShared(&g_managedIndexRegistryLock);
    for (ManagedIndexBufferCompat* buffer : g_managedIndexBuffers) {
        if (buffer != nullptr && buffer->Inner() == inner) {
            buffer->AddRef();
            result = buffer;
            break;
        }
    }
    ReleaseSRWLockShared(&g_managedIndexRegistryLock);
    return result;
}

bool CanTrackBoundManagedIndexBuffer(
    IDirect3DDevice9* device,
    ManagedIndexBufferCompat* retainedBuffer) noexcept {
    if (device == nullptr || retainedBuffer == nullptr) {
        return true;
    }
    bool canTrack = false;
    AcquireSRWLockShared(&g_boundManagedIndexLock);
    for (const BoundManagedIndexBuffer& record :
         g_boundManagedIndexBuffers) {
        if (record.device == device || record.device == nullptr) {
            canTrack = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_boundManagedIndexLock);
    return canTrack;
}

ManagedIndexBufferCompat* TakeBoundManagedIndexBuffer(
    IDirect3DDevice9* device) noexcept {
    if (device == nullptr) {
        return nullptr;
    }
    ManagedIndexBufferCompat* retainedBuffer = nullptr;
    AcquireSRWLockExclusive(&g_boundManagedIndexLock);
    for (BoundManagedIndexBuffer& record : g_boundManagedIndexBuffers) {
        if (record.device == device) {
            retainedBuffer = record.buffer;
            record = {};
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_boundManagedIndexLock);
    return retainedBuffer;
}

void ReplaceBoundManagedIndexBufferLocked(
    IDirect3DDevice9* device,
    ManagedIndexBufferCompat* retainedBuffer) noexcept {
    if (device == nullptr) {
        if (retainedBuffer != nullptr) {
            retainedBuffer->ReleaseFromBinding();
        }
        return;
    }

    BoundManagedIndexBuffer* selected = nullptr;
    BoundManagedIndexBuffer* empty = nullptr;
    ManagedIndexBufferCompat* previous = nullptr;
    AcquireSRWLockExclusive(&g_boundManagedIndexLock);
    for (BoundManagedIndexBuffer& record : g_boundManagedIndexBuffers) {
        if (record.device == device) {
            selected = &record;
            break;
        }
        if (empty == nullptr && record.device == nullptr) {
            empty = &record;
        }
    }
    if (selected == nullptr && retainedBuffer != nullptr) {
        selected = empty;
    }
    if (selected != nullptr) {
        previous = selected->buffer;
        if (retainedBuffer == nullptr) {
            *selected = {};
        } else {
            selected->device = device;
            selected->buffer = retainedBuffer;
        }
    }
    ReleaseSRWLockExclusive(&g_boundManagedIndexLock);

    if (previous != nullptr) {
        previous->ReleaseFromBinding();
    }
    if (retainedBuffer != nullptr && selected == nullptr) {
        retainedBuffer->ReleaseFromBinding();
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_bound_index_registry_full",
            "A managed index binding could not be tracked because the "
            "per-device binding table is full.");
    }
}

void ReplaceBoundManagedIndexBuffer(
    IDirect3DDevice9* device,
    ManagedIndexBufferCompat* retainedBuffer) noexcept {
    std::lock_guard<std::recursive_mutex> lock(
        g_managedIndexBindingCallMutex);
    ReplaceBoundManagedIndexBufferLocked(device, retainedBuffer);
}

void LogManagedIndexSetIndicesLifecycle(
    IDirect3DIndexBuffer9* indexBuffer, HRESULT result,
    bool unwrapped) noexcept {
    if (indexBuffer == nullptr) {
        return;
    }
    AcquireSRWLockShared(&g_managedIndexRegistryLock);
    for (ManagedIndexBufferCompat* buffer : g_managedIndexBuffers) {
        if (static_cast<IDirect3DIndexBuffer9*>(buffer) == indexBuffer) {
            buffer->LogSetIndicesLifecycle(result, unwrapped);
            break;
        }
    }
    ReleaseSRWLockShared(&g_managedIndexRegistryLock);
}

void PrepareManagedIndexBuffersForReset() noexcept {
    std::array<
        ManagedIndexBufferCompat*, kManagedIndexBufferCapacity> buffers{};
    AcquireSRWLockShared(&g_managedIndexRegistryLock);
    for (std::size_t index = 0;
         index < g_managedIndexBuffers.size(); ++index) {
        buffers[index] = g_managedIndexBuffers[index];
        if (buffers[index] != nullptr) {
            buffers[index]->RetainForSnapshot();
        }
    }
    ReleaseSRWLockShared(&g_managedIndexRegistryLock);

    UINT prepared = 0;
    UINT registered = 0;
    UINT locked = 0;
    UINT shadowed = 0;
    HRESULT failure = S_OK;
    for (ManagedIndexBufferCompat* buffer : buffers) {
        if (buffer == nullptr) {
            continue;
        }
        ++registered;
        locked += buffer->IsLocked() ? 1U : 0U;
        shadowed += buffer->HasShadow() ? 1U : 0U;
        buffer->LogResetSnapshot("before_prepare", S_OK);
        const HRESULT result = buffer->PrepareForReset();
        buffer->LogResetSnapshot("after_prepare", result);
        if (result == S_OK) {
            ++prepared;
        } else if (FAILED(result) && SUCCEEDED(failure)) {
            failure = result;
        }
        buffer->ReleaseFromSnapshot();
    }
    std::ostringstream message;
    message << "registered=" << registered
            << " locked=" << locked
            << " shadowed=" << shadowed
            << " prepared=" << prepared << " HRESULT=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(failure);
    GetBridge().LogHookStatus(
        FAILED(failure) ? "ERROR" : "INFO",
        "d3d9ex_managed_prepare_reset", message.str());
}

void RestoreManagedIndexBuffersAfterReset() noexcept {
    std::array<
        ManagedIndexBufferCompat*, kManagedIndexBufferCapacity> buffers{};
    AcquireSRWLockShared(&g_managedIndexRegistryLock);
    for (std::size_t index = 0;
         index < g_managedIndexBuffers.size(); ++index) {
        buffers[index] = g_managedIndexBuffers[index];
        if (buffers[index] != nullptr) {
            buffers[index]->RetainForSnapshot();
        }
    }
    ReleaseSRWLockShared(&g_managedIndexRegistryLock);

    UINT restored = 0;
    UINT registered = 0;
    UINT shadowed = 0;
    HRESULT failure = S_OK;
    for (ManagedIndexBufferCompat* buffer : buffers) {
        if (buffer == nullptr) {
            continue;
        }
        ++registered;
        shadowed += buffer->HasShadow() ? 1U : 0U;
        buffer->LogResetSnapshot("before_restore", S_OK);
        const HRESULT result = buffer->RestoreAfterReset();
        buffer->LogResetSnapshot("after_restore", result);
        if (result == S_OK) {
            ++restored;
        } else if (FAILED(result) && SUCCEEDED(failure)) {
            failure = result;
        }
        buffer->ReleaseFromSnapshot();
    }
    std::ostringstream message;
    message << "registered=" << registered
            << " shadowed=" << shadowed
            << " restored=" << restored << " HRESULT=0x"
            << std::hex << std::uppercase
            << static_cast<std::uint32_t>(failure);
    GetBridge().LogHookStatus(
        FAILED(failure) ? "ERROR" : "INFO",
        "d3d9ex_managed_restore", message.str());
}

void LogManagedResourceResult(
    std::size_t kindIndex, const char* kind, HRESULT result) noexcept {
    if (FAILED(result)) {
        std::ostringstream message;
        message << "kind=" << kind << " HRESULT=0x"
                << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_managed_create_failed", message.str());
        return;
    }
    if (kindIndex < 5 &&
        InterlockedCompareExchange(
            &g_managedResourceSuccessLogged[kindIndex],
            TRUE, FALSE) == FALSE) {
        GetBridge().LogHookStatus(
            "INFO", "d3d9ex_managed_create",
            std::string("kind=") + kind + " HRESULT=0x00000000");
    }
}


HRESULT STDMETHODCALLTYPE HookCreateDevice(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** output);
HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(
    IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode,
    IDirect3DDevice9Ex** output);
ULONG STDMETHODCALLTYPE HookDeviceRelease(IDirect3DDevice9* self);
HRESULT STDMETHODCALLTYPE HookCreateAdditionalSwapChain(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DSwapChain9** output);
HRESULT STDMETHODCALLTYPE HookGetSwapChain(
    IDirect3DDevice9* self, UINT index,
    IDirect3DSwapChain9** output);
HRESULT STDMETHODCALLTYPE HookCreateTexture(
    IDirect3DDevice9* self, UINT width, UINT height, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** output, HANDLE* sharedHandle);
HRESULT STDMETHODCALLTYPE HookCreateVolumeTexture(
    IDirect3DDevice9* self, UINT width, UINT height, UINT depth,
    UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9** output, HANDLE* sharedHandle);
HRESULT STDMETHODCALLTYPE HookCreateCubeTexture(
    IDirect3DDevice9* self, UINT edgeLength, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** output,
    HANDLE* sharedHandle);
HRESULT STDMETHODCALLTYPE HookCreateVertexBuffer(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf,
    D3DPOOL pool, IDirect3DVertexBuffer9** output,
    HANDLE* sharedHandle);
HRESULT STDMETHODCALLTYPE HookCreateIndexBuffer(
    IDirect3DDevice9* self, UINT length, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** output,
    HANDLE* sharedHandle);
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self,
                                     D3DPRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self,
                                       const RECT* source,
                                       const RECT* destination,
                                       HWND overrideWindow,
                                       const RGNDATA* dirtyRegion);
HRESULT STDMETHODCALLTYPE HookResetEx(
    IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode);
HRESULT STDMETHODCALLTYPE HookPresentEx(
    IDirect3DDevice9Ex* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion, DWORD flags);
HRESULT STDMETHODCALLTYPE HookSwapChainPresent(
    IDirect3DSwapChain9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion, DWORD flags);
HRESULT STDMETHODCALLTYPE HookAppLocalReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE HookAppLocalPresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion);
HRESULT STDMETHODCALLTYPE HookAppLocalSetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9* indexBuffer);
HRESULT STDMETHODCALLTYPE HookAppLocalGetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9** indexBuffer);

HRESULT STDMETHODCALLTYPE HookSetViewport(
    IDirect3DDevice9* self, const D3DVIEWPORT9* viewport);
HRESULT STDMETHODCALLTYPE HookSetScissorRect(
    IDirect3DDevice9* self, const RECT* rectangle);
HRESULT STDMETHODCALLTYPE HookLateReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE HookLatePresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion);
HRESULT STDMETHODCALLTYPE HookLateSetViewport(
    IDirect3DDevice9* self, const D3DVIEWPORT9* viewport);
HRESULT STDMETHODCALLTYPE HookLateSetScissorRect(
    IDirect3DDevice9* self, const RECT* rectangle);

bool ReplaceVtableEntry(void** vtable, std::size_t index,
                        void* replacement) noexcept {
    DWORD oldProtection = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&vtable[index]), replacement);
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index],
                          sizeof(void*));
    return true;
}

bool InstallAppLocalCompatibilityHooks(
    void* resetTarget, void* presentTarget,
    void* setIndicesTarget, void* getIndicesTarget) noexcept {
    if (resetTarget == nullptr || presentTarget == nullptr ||
        setIndicesTarget == nullptr || getIndicesTarget == nullptr) {
        return false;
    }

    AcquireSRWLockExclusive(&g_appLocalHookLock);
    if (InterlockedCompareExchange(
            &g_appLocalHooksActive, FALSE, FALSE) != FALSE) {
        const bool targetsMatch =
            g_appLocalReset != nullptr &&
            g_appLocalPresent != nullptr &&
            g_appLocalSetIndices != nullptr &&
            g_appLocalGetIndices != nullptr &&
            g_appLocalResetTarget == resetTarget &&
            g_appLocalPresentTarget == presentTarget &&
            g_appLocalSetIndicesTarget == setIndicesTarget &&
            g_appLocalGetIndicesTarget == getIndicesTarget;
        ReleaseSRWLockExclusive(&g_appLocalHookLock);
        if (!targetsMatch) {
            GetBridge().LogHookStatus(
                "ERROR", "app_local_hook_target_mismatch",
                "The active compatibility detours belong to a different "
                "D3D9 device implementation.");
        }
        return targetsMatch;
    }
    if (InterlockedCompareExchange(
            &g_appLocalHooksPoisoned, FALSE, FALSE) != FALSE) {
        ReleaseSRWLockExclusive(&g_appLocalHookLock);
        GetBridge().LogHookStatus(
            "ERROR", "app_local_hook_rollback_incomplete",
            "A previous compatibility-detour rollback was incomplete; "
            "another D3D9Ex device was rejected safely.");
        return false;
    }

    g_appLocalResetTarget = resetTarget;
    g_appLocalPresentTarget = presentTarget;
    g_appLocalSetIndicesTarget = setIndicesTarget;
    g_appLocalGetIndicesTarget = getIndicesTarget;

    const auto clearState = [&]() {
        g_appLocalReset = nullptr;
        g_appLocalPresent = nullptr;
        g_appLocalSetIndices = nullptr;
        g_appLocalGetIndices = nullptr;
        g_appLocalResetTarget = nullptr;
        g_appLocalPresentTarget = nullptr;
        g_appLocalSetIndicesTarget = nullptr;
        g_appLocalGetIndicesTarget = nullptr;
    };
    const auto rollbackHook = [](void* target) {
        const MH_STATUS disable = MH_DisableHook(target);
        const bool disabled =
            disable == MH_OK || disable == MH_ERROR_DISABLED ||
            disable == MH_ERROR_NOT_CREATED;
        const MH_STATUS remove = MH_RemoveHook(target);
        const bool removed =
            remove == MH_OK || remove == MH_ERROR_NOT_CREATED;
        return disabled && removed;
    };
    const auto fail = [&](const char* event, MH_STATUS status,
                          bool rollbackClean) {
        InterlockedExchange(&g_appLocalHooksActive, FALSE);
        InterlockedExchange(
            &g_appLocalHooksPoisoned,
            rollbackClean ? FALSE : TRUE);
        if (rollbackClean) {
            clearState();
        }
        ReleaseSRWLockExclusive(&g_appLocalHookLock);
        std::string detail = MH_StatusToString(status);
        if (!rollbackClean) {
            detail += "; rollback incomplete, detours are pass-through";
        }
        GetBridge().LogHookStatus(
            "ERROR", event, detail);
        return false;
    };

    const MH_STATUS initialize = MH_Initialize();
    if (initialize != MH_OK &&
        initialize != MH_ERROR_ALREADY_INITIALIZED) {
        return fail(
            "app_local_hook_initialize_failed", initialize, true);
    }

    MH_STATUS status = MH_CreateHook(
        resetTarget, reinterpret_cast<void*>(&HookAppLocalReset),
        reinterpret_cast<void**>(&g_appLocalReset));
    if (status != MH_OK) {
        return fail("app_local_reset_hook_failed", status, true);
    }
    status = MH_CreateHook(
        presentTarget, reinterpret_cast<void*>(&HookAppLocalPresent),
        reinterpret_cast<void**>(&g_appLocalPresent));
    if (status != MH_OK) {
        const bool rollbackClean = rollbackHook(resetTarget);
        return fail(
            "app_local_present_hook_failed", status, rollbackClean);
    }
    status = MH_CreateHook(
        setIndicesTarget,
        reinterpret_cast<void*>(&HookAppLocalSetIndices),
        reinterpret_cast<void**>(&g_appLocalSetIndices));
    if (status != MH_OK) {
        bool rollbackClean = rollbackHook(presentTarget);
        rollbackClean = rollbackHook(resetTarget) && rollbackClean;
        return fail(
            "app_local_set_indices_hook_failed", status,
            rollbackClean);
    }
    status = MH_CreateHook(
        getIndicesTarget,
        reinterpret_cast<void*>(&HookAppLocalGetIndices),
        reinterpret_cast<void**>(&g_appLocalGetIndices));
    if (status != MH_OK) {
        bool rollbackClean = rollbackHook(setIndicesTarget);
        rollbackClean = rollbackHook(presentTarget) && rollbackClean;
        rollbackClean = rollbackHook(resetTarget) && rollbackClean;
        return fail(
            "app_local_get_indices_hook_failed", status,
            rollbackClean);
    }

    status = MH_QueueEnableHook(resetTarget);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(presentTarget);
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(setIndicesTarget);
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(getIndicesTarget);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        bool rollbackClean = rollbackHook(getIndicesTarget);
        rollbackClean = rollbackHook(setIndicesTarget) && rollbackClean;
        rollbackClean = rollbackHook(presentTarget) && rollbackClean;
        rollbackClean = rollbackHook(resetTarget) && rollbackClean;
        return fail(
            "app_local_hook_enable_failed", status, rollbackClean);
    }

    InterlockedExchange(&g_appLocalHooksPoisoned, FALSE);
    InterlockedExchange(&g_appLocalHooksActive, TRUE);
    ReleaseSRWLockExclusive(&g_appLocalHookLock);
    GetBridge().LogHookStatus(
        "INFO", "app_local_d3d9ex_hooks_active",
        "Reset, Present, SetIndices, and GetIndices implementation hooks "
        "are active for D3D9Ex compatibility.");
    return true;
}

bool PatchSwapChain(IDirect3DSwapChain9* swapChain) noexcept {
    if (swapChain == nullptr) {
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    bool patched = false;
    AcquireSRWLockExclusive(&g_hookLock);
    for (const SwapChainVtableRecord& record : g_swapChainRecords) {
        if (record.vtable == vtable) {
            const bool active = record.present != nullptr;
            ReleaseSRWLockExclusive(&g_hookLock);
            return active;
        }
    }
    for (SwapChainVtableRecord& record : g_swapChainRecords) {
        if (record.vtable != nullptr) {
            continue;
        }
        record.vtable = vtable;
        record.present =
            reinterpret_cast<SwapChainPresentFunction>(vtable[3]);
        patched = ReplaceVtableEntry(
            vtable, 3,
            reinterpret_cast<void*>(&HookSwapChainPresent));
        if (!patched) {
            record = {};
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_hookLock);

    if (patched &&
        InterlockedCompareExchange(
            &g_swapChainHookLogged, TRUE, FALSE) == FALSE) {
        GetBridge().LogHookStatus(
            "INFO", "swapchain_present_hooked",
            "IDirect3DSwapChain9::Present capture is active.");
    }
    if (!patched) {
        GetBridge().LogHookStatus(
            "ERROR", "swapchain_present_hook_failed",
            "IDirect3DSwapChain9::Present could not be intercepted safely.");
    }
    return patched;
}

bool PatchDevice(
    IDirect3DDevice9* device, bool hasEx = false,
    bool translateManagedResources = false) noexcept {
    if (device == nullptr) {
        return false;
    }
    const bool lateHooksActive =
        InterlockedCompareExchange(
            &g_lateHooksActive, FALSE, FALSE) != FALSE;
    if (lateHooksActive) {
        return !translateManagedResources;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    const bool implementationHooksActive =
        translateManagedResources &&
        InstallAppLocalCompatibilityHooks(
            vtable[16], vtable[17], vtable[104], vtable[105]);
    if (translateManagedResources && !implementationHooksActive) {
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_required_hooks_missing",
            "D3D9Ex compatibility was rejected because Reset, Present, "
            "SetIndices, or GetIndices could not be intercepted safely.");
        return false;
    }

    AcquireSRWLockExclusive(&g_hookLock);
    DeviceVtableRecord* selected = nullptr;
    for (DeviceVtableRecord& record : g_deviceRecords) {
        if (record.vtable == vtable) {
            selected = &record;
            break;
        }
    }
    if (selected == nullptr) {
        for (DeviceVtableRecord& record : g_deviceRecords) {
            if (record.vtable != nullptr) {
                continue;
            }
            selected = &record;
            selected->vtable = vtable;
            selected->addRef =
                reinterpret_cast<DeviceAddRefFunction>(vtable[1]);
            selected->release =
                reinterpret_cast<DeviceReleaseFunction>(vtable[2]);
            selected->createAdditionalSwapChain =
                reinterpret_cast<CreateAdditionalSwapChainFunction>(
                    vtable[13]);
            selected->getSwapChain =
                reinterpret_cast<GetSwapChainFunction>(vtable[14]);
            selected->reset = implementationHooksActive
                ? g_appLocalReset
                : reinterpret_cast<ResetFunction>(vtable[16]);
            selected->present = implementationHooksActive
                ? g_appLocalPresent
                : reinterpret_cast<PresentFunction>(vtable[17]);
            selected->setViewport =
                reinterpret_cast<SetViewportFunction>(vtable[47]);
            selected->setScissorRect =
                reinterpret_cast<SetScissorRectFunction>(vtable[75]);
            bool baseHooksActive = true;
            baseHooksActive = ReplaceVtableEntry(
                vtable, 2,
                reinterpret_cast<void*>(&HookDeviceRelease)) &&
                baseHooksActive;
            baseHooksActive = ReplaceVtableEntry(
                vtable, 13,
                reinterpret_cast<void*>(
                    &HookCreateAdditionalSwapChain)) &&
                baseHooksActive;
            baseHooksActive = ReplaceVtableEntry(
                vtable, 14,
                reinterpret_cast<void*>(&HookGetSwapChain)) &&
                baseHooksActive;
            if (!implementationHooksActive) {
                baseHooksActive = ReplaceVtableEntry(
                    vtable, 16,
                    reinterpret_cast<void*>(&HookReset)) &&
                    baseHooksActive;
                baseHooksActive = ReplaceVtableEntry(
                    vtable, 17,
                    reinterpret_cast<void*>(&HookPresent)) &&
                    baseHooksActive;
            }
            baseHooksActive = ReplaceVtableEntry(
                vtable, 47,
                reinterpret_cast<void*>(&HookSetViewport)) &&
                baseHooksActive;
            baseHooksActive = ReplaceVtableEntry(
                vtable, 75,
                reinterpret_cast<void*>(&HookSetScissorRect)) &&
                baseHooksActive;
            selected->baseHooksActive = baseHooksActive;
            break;
        }
    }

    if (selected != nullptr && translateManagedResources &&
        !selected->managedResourceHooksActive) {
        if (selected->createTexture == nullptr) {
            selected->createTexture =
                reinterpret_cast<CreateTextureFunction>(vtable[23]);
            selected->createVolumeTexture =
                reinterpret_cast<CreateVolumeTextureFunction>(vtable[24]);
            selected->createCubeTexture =
                reinterpret_cast<CreateCubeTextureFunction>(vtable[25]);
            selected->createVertexBuffer =
                reinterpret_cast<CreateVertexBufferFunction>(vtable[26]);
            selected->createIndexBuffer =
                reinterpret_cast<CreateIndexBufferFunction>(vtable[27]);
        }
        bool resourceHooksActive = ReplaceVtableEntry(
            vtable, 23,
            reinterpret_cast<void*>(&HookCreateTexture));
        resourceHooksActive = ReplaceVtableEntry(
            vtable, 24,
            reinterpret_cast<void*>(&HookCreateVolumeTexture)) &&
            resourceHooksActive;
        resourceHooksActive = ReplaceVtableEntry(
            vtable, 25,
            reinterpret_cast<void*>(&HookCreateCubeTexture)) &&
            resourceHooksActive;
        resourceHooksActive = ReplaceVtableEntry(
            vtable, 26,
            reinterpret_cast<void*>(&HookCreateVertexBuffer)) &&
            resourceHooksActive;
        resourceHooksActive = ReplaceVtableEntry(
            vtable, 27,
            reinterpret_cast<void*>(&HookCreateIndexBuffer)) &&
            resourceHooksActive;
        selected->managedResourceHooksActive = resourceHooksActive;
    }

    if (selected != nullptr && hasEx &&
        !selected->exHooksActive) {
        if (selected->presentEx == nullptr) {
            selected->presentEx =
                reinterpret_cast<PresentExFunction>(vtable[121]);
            selected->resetEx =
                reinterpret_cast<ResetExFunction>(vtable[132]);
        }
        bool exHooksActive = ReplaceVtableEntry(
            vtable, 121,
            reinterpret_cast<void*>(&HookPresentEx));
        exHooksActive = ReplaceVtableEntry(
            vtable, 132,
            reinterpret_cast<void*>(&HookResetEx)) &&
            exHooksActive;
        selected->exHooksActive = exHooksActive;
    }
    const bool deviceRecorded = selected != nullptr;
    const bool compatibilityReady =
        deviceRecorded && selected->baseHooksActive &&
        (!translateManagedResources ||
         (selected->managedResourceHooksActive &&
          (!hasEx || selected->exHooksActive)));
    const bool baseHookFailure =
        deviceRecorded && !selected->baseHooksActive;
    const bool managedHookFailure =
        deviceRecorded && translateManagedResources &&
        !selected->managedResourceHooksActive;
    const bool exHookFailure =
        deviceRecorded && translateManagedResources && hasEx &&
        !selected->exHooksActive;
    ReleaseSRWLockExclusive(&g_hookLock);

    if (baseHookFailure || managedHookFailure || exHookFailure) {
        std::string missing;
        if (baseHookFailure) {
            missing += "device release/swap-chain/viewport/scissor/present;";
        }
        if (managedHookFailure) {
            missing += "managed-resource creation;";
        }
        if (exHookFailure) {
            missing += "ResetEx/PresentEx;";
        }
        GetBridge().LogHookStatus(
            translateManagedResources ? "ERROR" : "WARN",
            "d3d9_vtable_patch_failed",
            "Required device hooks could not be installed: " + missing);
    }

    bool defaultSwapChainReady = false;
    IDirect3DSwapChain9* defaultSwapChain = nullptr;
    if (deviceRecorded &&
        SUCCEEDED(device->GetSwapChain(0, &defaultSwapChain)) &&
        defaultSwapChain != nullptr) {
        defaultSwapChainReady = PatchSwapChain(defaultSwapChain);
        defaultSwapChain->Release();
    }
    if (translateManagedResources && !defaultSwapChainReady) {
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_default_swapchain_hook_failed",
            "D3D9Ex compatibility was rejected because the default "
            "swap-chain Present path could not be intercepted safely.");
    }
    return compatibilityReady &&
        (!translateManagedResources || defaultSwapChainReady);
}

bool PatchD3D9(IDirect3D9* direct3D, bool hasEx) noexcept {
    if (direct3D == nullptr) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(direct3D);
    AcquireSRWLockExclusive(&g_hookLock);
    D3D9VtableRecord* selected = nullptr;
    for (D3D9VtableRecord& record : g_d3d9Records) {
        if (record.vtable == vtable) {
            selected = &record;
            break;
        }
    }
    if (selected == nullptr) {
        for (D3D9VtableRecord& record : g_d3d9Records) {
            if (record.vtable == nullptr) {
                selected = &record;
                selected->vtable = vtable;
                selected->createDevice =
                    reinterpret_cast<CreateDeviceFunction>(vtable[16]);
                break;
            }
        }
    }
    if (selected != nullptr && !selected->createDeviceHookActive) {
        selected->createDeviceHookActive = ReplaceVtableEntry(
            vtable, 16, reinterpret_cast<void*>(&HookCreateDevice));
    }
    if (selected != nullptr && hasEx &&
        !selected->createDeviceExHookActive) {
        if (selected->createDeviceEx == nullptr) {
            selected->createDeviceEx =
                reinterpret_cast<CreateDeviceExFunction>(vtable[20]);
        }
        selected->createDeviceExHookActive = ReplaceVtableEntry(
            vtable, 20,
            reinterpret_cast<void*>(&HookCreateDeviceEx));
    }
    const bool patched =
        selected != nullptr && selected->createDeviceHookActive &&
        (!hasEx || selected->createDeviceExHookActive);
    ReleaseSRWLockExclusive(&g_hookLock);
    if (!patched) {
        GetBridge().LogHookStatus(
            "ERROR", "d3d9_factory_vtable_patch_failed",
            "The Direct3D factory could not be intercepted safely.");
    }
    return patched;
}

D3D9VtableRecord FindD3D9Record(void** vtable) noexcept {
    D3D9VtableRecord result;
    AcquireSRWLockShared(&g_hookLock);
    for (const D3D9VtableRecord& record : g_d3d9Records) {
        if (record.vtable == vtable) {
            result = record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hookLock);
    return result;
}

DeviceVtableRecord FindDeviceRecord(void** vtable) noexcept {
    DeviceVtableRecord result;
    AcquireSRWLockShared(&g_hookLock);
    for (const DeviceVtableRecord& record : g_deviceRecords) {
        if (record.vtable == vtable) {
            result = record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hookLock);
    return result;
}

SwapChainVtableRecord FindSwapChainRecord(void** vtable) noexcept {
    SwapChainVtableRecord result;
    AcquireSRWLockShared(&g_hookLock);
    for (const SwapChainVtableRecord& record : g_swapChainRecords) {
        if (record.vtable == vtable) {
            result = record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hookLock);
    return result;
}

ULONG STDMETHODCALLTYPE HookDeviceRelease(IDirect3DDevice9* self) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.release == nullptr) {
        GetBridge().LogHookStatus(
            "ERROR", "d3d9_device_release_record_missing",
            "A hooked device Release call had no original target.");
        return 0;
    }

    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    ManagedIndexBufferCompat* detachedBinding = nullptr;
    if (record.addRef != nullptr) {
        // Probe without changing the net count. When the caller owns the
        // final external device reference and our bound inner resource owns
        // the other one, detach the wrapper before the caller's Release.
        // Releasing the wrapper can recursively release the device, which is
        // why this path is serialized by a recursive mutex.
        record.addRef(self);
        const ULONG referencesBeforeRelease = record.release(self);
        if (referencesBeforeRelease == 2) {
            detachedBinding = TakeBoundManagedIndexBuffer(self);
            if (detachedBinding != nullptr) {
                detachedBinding->ReleaseFromBinding();
            }
        }
    }
    const ULONG remaining = record.release(self);
    if (remaining == 0 && detachedBinding == nullptr) {
        ManagedIndexBufferCompat* orphanedBinding =
            TakeBoundManagedIndexBuffer(self);
        if (orphanedBinding != nullptr) {
            // A native D3D9 resource is expected to hold its parent device,
            // so this state should be unreachable. Remove the stale device
            // key but retain the wrapper rather than releasing a resource
            // after its parent device has already been destroyed.
            orphanedBinding->QuarantineAfterDeviceLoss();
            GetBridge().LogHookStatus(
                "ERROR", "d3d9ex_bound_index_release_order_invalid",
                "A managed index binding outlived its device; the stale "
                "device key and reset registry entry were removed, while "
                "the wrapper was retained to avoid touching its dead parent.");
        }
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** output) {
    const D3D9VtableRecord record =
        FindD3D9Record(*reinterpret_cast<void***>(self));
    if (record.createDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    ForceHighQualitySource(parameters);
    const HRESULT result = record.createDevice(
        self, adapter, deviceType, focusWindow, behaviorFlags, parameters,
        output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) {
        PatchDevice(*output, false, false);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(
    IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode,
    IDirect3DDevice9Ex** output) {
    const D3D9VtableRecord record =
        FindD3D9Record(*reinterpret_cast<void***>(self));
    if (record.createDeviceEx == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    ForceHighQualitySource(parameters);
    HRESULT result = record.createDeviceEx(
        self, adapter, deviceType, focusWindow, behaviorFlags, parameters,
        fullscreenMode, output);
    const bool compatibilityRequested =
        D3D9ExCompatibilityRequested();
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr &&
        !PatchDevice(*output, true, compatibilityRequested) &&
        compatibilityRequested) {
        (*output)->Release();
        *output = nullptr;
        result = D3DERR_NOTAVAILABLE;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateAdditionalSwapChain(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DSwapChain9** output) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createAdditionalSwapChain == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    HRESULT result =
        record.createAdditionalSwapChain(self, parameters, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) {
        const bool patched = PatchSwapChain(*output);
        if (!patched && D3D9ExCompatibilityRequested()) {
            (*output)->Release();
            *output = nullptr;
            result = D3DERR_NOTAVAILABLE;
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetSwapChain(
    IDirect3DDevice9* self, UINT index,
    IDirect3DSwapChain9** output) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.getSwapChain == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    HRESULT result = record.getSwapChain(self, index, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) {
        const bool patched = PatchSwapChain(*output);
        if (!patched && D3D9ExCompatibilityRequested()) {
            (*output)->Release();
            *output = nullptr;
            result = D3DERR_NOTAVAILABLE;
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture(
    IDirect3DDevice9* self, UINT width, UINT height, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** output, HANDLE* sharedHandle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createTexture == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const bool translated =
        TranslateManagedResource(usage, pool, true);
    const HRESULT result = record.createTexture(
        self, width, height, levels, usage, format, pool, output,
        sharedHandle);
    if (translated) {
        LogManagedResourceResult(0, "texture", result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateVolumeTexture(
    IDirect3DDevice9* self, UINT width, UINT height, UINT depth,
    UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9** output, HANDLE* sharedHandle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createVolumeTexture == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const bool translated =
        TranslateManagedResource(usage, pool, true);
    const HRESULT result = record.createVolumeTexture(
        self, width, height, depth, levels, usage, format, pool,
        output, sharedHandle);
    if (translated) {
        LogManagedResourceResult(1, "volume_texture", result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateCubeTexture(
    IDirect3DDevice9* self, UINT edgeLength, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** output,
    HANDLE* sharedHandle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createCubeTexture == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const bool translated =
        TranslateManagedResource(usage, pool, true);
    const HRESULT result = record.createCubeTexture(
        self, edgeLength, levels, usage, format, pool, output,
        sharedHandle);
    if (translated) {
        LogManagedResourceResult(2, "cube_texture", result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateVertexBuffer(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf,
    D3DPOOL pool, IDirect3DVertexBuffer9** output,
    HANDLE* sharedHandle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createVertexBuffer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const bool translated =
        TranslateManagedResource(usage, pool, false);
    const HRESULT result = record.createVertexBuffer(
        self, length, usage, fvf, pool, output, sharedHandle);
    if (translated) {
        LogManagedResourceResult(3, "vertex_buffer", result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateIndexBuffer(
    IDirect3DDevice9* self, UINT length, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** output,
    HANDLE* sharedHandle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.createIndexBuffer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const DWORD requestedUsage = usage;
    const D3DPOOL requestedPool = pool;
    const bool translated =
        TranslateManagedResource(usage, pool, false);
    if (output != nullptr) {
        *output = nullptr;
    }
    IDirect3DIndexBuffer9* inner = nullptr;
    HRESULT result = record.createIndexBuffer(
        self, length, usage, format, pool, &inner, sharedHandle);
    if (translated && SUCCEEDED(result) && inner != nullptr) {
        ManagedIndexBufferCompat* compatibility = nullptr;
        try {
            compatibility = new (std::nothrow)
                ManagedIndexBufferCompat(
                    inner, requestedUsage, format, length);
        } catch (const std::bad_alloc&) {
            compatibility = nullptr;
        }
        if (compatibility == nullptr) {
            inner->Release();
            result = E_OUTOFMEMORY;
        } else if (!compatibility->Registered()) {
            compatibility->Release();
            result = E_OUTOFMEMORY;
        } else if (output == nullptr) {
            compatibility->Release();
            result = D3DERR_INVALIDCALL;
        } else {
            *output = compatibility;
        }
    } else if (output != nullptr) {
        *output = inner;
    } else if (inner != nullptr) {
        inner->Release();
        result = D3DERR_INVALIDCALL;
    }
    if (translated) {
        LogManagedResourceResult(4, "index_buffer", result);
        if (InterlockedCompareExchange(
                &g_managedIndexCreateDetailsLogged,
                TRUE, FALSE) == FALSE) {
            std::ostringstream message;
            message << "length=" << length
                    << " requested_usage=0x" << std::hex
                    << requestedUsage
                    << " effective_usage=0x" << usage << std::dec
                    << " requested_pool="
                    << static_cast<unsigned>(requestedPool)
                    << " effective_pool="
                    << static_cast<unsigned>(pool)
                    << " format=" << static_cast<unsigned>(format)
                    << " HRESULT=0x" << std::hex << std::uppercase
                    << static_cast<std::uint32_t>(result);
            GetBridge().LogHookStatus(
                SUCCEEDED(result) ? "INFO" : "ERROR",
                "d3d9ex_managed_index_create_details",
                message.str());
        }
    }
    return result;
}


HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self,
                                     D3DPRESENT_PARAMETERS* parameters) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.reset == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    if (D3D9ExCompatibilityRequested() &&
        !D3D9ExExclusiveRequested() && parameters != nullptr) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        HWND focusWindow = nullptr;
        if (SUCCEEDED(self->GetCreationParameters(&creation))) {
            focusWindow = creation.hFocusWindow;
        }
        TranslateD3D9ExPresentation(focusWindow, parameters);
    }
    ForceHighQualitySource(parameters);
    if (D3D9ExCompatibilityRequested()) {
        PrepareManagedIndexBuffersForReset();
    }
    GetBridge().BeforeReset();
    const HRESULT result = record.reset(self, parameters);
    if (SUCCEEDED(result) && D3D9ExCompatibilityRequested()) {
        ReplaceBoundManagedIndexBuffer(self, nullptr);
        RestoreManagedIndexBuffersAfterReset();
    }
    GetBridge().AfterReset(result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self,
                                       const RECT* source,
                                       const RECT* destination,
                                       HWND overrideWindow,
                                       const RGNDATA* dirtyRegion) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.present == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    InterlockedIncrement64(&g_presentHookCounts[0]);
    GetBridge().CapturePresent(self);
    return record.present(self, source, destination, overrideWindow,
                          dirtyRegion);
}

HRESULT STDMETHODCALLTYPE HookResetEx(
    IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.resetEx == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    bool translated = false;
    if (D3D9ExCompatibilityRequested() &&
        !D3D9ExExclusiveRequested() && parameters != nullptr) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        HWND focusWindow = nullptr;
        if (SUCCEEDED(self->GetCreationParameters(&creation))) {
            focusWindow = creation.hFocusWindow;
        }
        translated =
            TranslateD3D9ExPresentation(focusWindow, parameters);
    }
    if (translated) {
        fullscreenMode = nullptr;
    }
    ForceHighQualitySource(parameters);
    if (D3D9ExCompatibilityRequested()) {
        PrepareManagedIndexBuffersForReset();
    }
    GetBridge().BeforeReset();
    const HRESULT result =
        record.resetEx(self, parameters, fullscreenMode);
    if (SUCCEEDED(result) && D3D9ExCompatibilityRequested()) {
        ReplaceBoundManagedIndexBuffer(self, nullptr);
        RestoreManagedIndexBuffersAfterReset();
    }
    GetBridge().AfterReset(result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresentEx(
    IDirect3DDevice9Ex* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion, DWORD flags) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.presentEx == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    InterlockedIncrement64(&g_presentHookCounts[1]);
    GetBridge().CapturePresent(self);
    return record.presentEx(
        self, source, destination, overrideWindow, dirtyRegion, flags);
}

HRESULT STDMETHODCALLTYPE HookSwapChainPresent(
    IDirect3DSwapChain9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion, DWORD flags) {
    const SwapChainVtableRecord record =
        FindSwapChainRecord(*reinterpret_cast<void***>(self));
    if (record.present == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    InterlockedIncrement64(&g_presentHookCounts[2]);
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DSurface9> backBuffer;
    if (SUCCEEDED(self->GetDevice(device.GetAddressOf())) && device &&
        SUCCEEDED(self->GetBackBuffer(
            0, D3DBACKBUFFER_TYPE_MONO,
            backBuffer.GetAddressOf())) &&
        backBuffer) {
        GetBridge().CapturePresent(device.Get(), backBuffer.Get());
    }
    return record.present(
        self, source, destination, overrideWindow, dirtyRegion, flags);
}

HRESULT STDMETHODCALLTYPE HookAppLocalReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    if (g_appLocalReset == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (InterlockedCompareExchange(
            &g_appLocalHooksActive, FALSE, FALSE) == FALSE) {
        return g_appLocalReset(self, parameters);
    }
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (D3D9ExCompatibilityRequested() &&
        !D3D9ExExclusiveRequested() && parameters != nullptr) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        HWND focusWindow = nullptr;
        if (SUCCEEDED(self->GetCreationParameters(&creation))) {
            focusWindow = creation.hFocusWindow;
        }
        TranslateD3D9ExPresentation(focusWindow, parameters);
    }
    ForceHighQualitySource(parameters);
    if (D3D9ExCompatibilityRequested()) {
        PrepareManagedIndexBuffersForReset();
    }
    GetBridge().BeforeReset();

    HRESULT result = D3DERR_INVALIDCALL;
    ComPtr<IDirect3DDevice9Ex> deviceEx;
    if (D3D9ExCompatibilityRequested() &&
        record.resetEx != nullptr &&
        SUCCEEDED(self->QueryInterface(
            __uuidof(IDirect3DDevice9Ex),
            reinterpret_cast<void**>(deviceEx.GetAddressOf()))) &&
        deviceEx) {
        D3DDISPLAYMODEEX displayMode{};
        D3DDISPLAYMODEEX* displayModePointer = nullptr;
        if (D3D9ExExclusiveRequested() && parameters != nullptr &&
            !parameters->Windowed) {
            displayMode.Size = sizeof(displayMode);
            if (FAILED(deviceEx->GetDisplayModeEx(
                    0, &displayMode, nullptr))) {
                displayMode.Width = parameters->BackBufferWidth;
                displayMode.Height = parameters->BackBufferHeight;
                displayMode.RefreshRate =
                    parameters->FullScreen_RefreshRateInHz;
                displayMode.Format = parameters->BackBufferFormat;
                displayMode.ScanLineOrdering =
                    D3DSCANLINEORDERING_PROGRESSIVE;
            } else {
                if (parameters->BackBufferWidth != 0) {
                    displayMode.Width =
                        parameters->BackBufferWidth;
                }
                if (parameters->BackBufferHeight != 0) {
                    displayMode.Height =
                        parameters->BackBufferHeight;
                }
                if (parameters->BackBufferFormat != D3DFMT_UNKNOWN) {
                    displayMode.Format =
                        parameters->BackBufferFormat;
                }
                if (parameters->FullScreen_RefreshRateInHz != 0) {
                    displayMode.RefreshRate =
                        parameters->FullScreen_RefreshRateInHz;
                }
            }
            parameters->FullScreen_RefreshRateInHz =
                displayMode.RefreshRate;
            displayModePointer = &displayMode;
        }
        result = record.resetEx(
            deviceEx.Get(), parameters, displayModePointer);
        GetBridge().LogHookStatus(
            SUCCEEDED(result) ? "INFO" : "ERROR",
            "d3d9ex_reset_redirected",
            SUCCEEDED(result)
                ? "Classic Reset completed through ResetEx."
                : "Classic Reset redirected to ResetEx but failed.");
    } else {
        result = g_appLocalReset(self, parameters);
    }
    if (SUCCEEDED(result) && D3D9ExCompatibilityRequested()) {
        ReplaceBoundManagedIndexBuffer(self, nullptr);
        RestoreManagedIndexBuffersAfterReset();
    }
    GetBridge().AfterReset(result);
    return result;
}


HRESULT STDMETHODCALLTYPE HookAppLocalPresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion) {
    if (g_appLocalPresent == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (InterlockedCompareExchange(
            &g_appLocalHooksActive, FALSE, FALSE) == FALSE) {
        return g_appLocalPresent(
            self, source, destination, overrideWindow, dirtyRegion);
    }
    InterlockedIncrement64(&g_presentHookCounts[0]);
    GetBridge().CapturePresent(self);
    return g_appLocalPresent(
        self, source, destination, overrideWindow, dirtyRegion);
}

HRESULT STDMETHODCALLTYPE HookAppLocalSetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9* indexBuffer) {
    if (g_appLocalSetIndices == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (InterlockedCompareExchange(
            &g_appLocalHooksActive, FALSE, FALSE) == FALSE) {
        return g_appLocalSetIndices(self, indexBuffer);
    }

    // D3DCREATE_MULTITHREADED serializes the runtime call, but the wrapper
    // ownership table is outside the runtime. Keep both operations in the
    // same order so GetIndices always observes the matching wrapper.
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    ManagedIndexBufferCompat* retainedWrapper =
        RetainManagedIndexBufferForBinding(indexBuffer);
    if (retainedWrapper != nullptr &&
        !CanTrackBoundManagedIndexBuffer(self, retainedWrapper)) {
        retainedWrapper->ReleaseFromBinding();
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_bound_index_registry_full",
            "SetIndices was rejected before the native binding changed "
            "because no wrapper-ownership slot was available.");
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    ComPtr<IDirect3DIndexBuffer9> inner;
    IDirect3DIndexBuffer9* effectiveBuffer = indexBuffer;
    if (indexBuffer != nullptr &&
        SUCCEEDED(indexBuffer->QueryInterface(
            kManagedIndexBufferInner,
            reinterpret_cast<void**>(inner.GetAddressOf()))) &&
        inner != nullptr) {
        effectiveBuffer = inner.Get();
    }
    const bool unwrapped = effectiveBuffer != indexBuffer;
    const HRESULT result =
        g_appLocalSetIndices(self, effectiveBuffer);
    LogManagedIndexSetIndicesLifecycle(
        indexBuffer, result, unwrapped);
    if (SUCCEEDED(result)) {
        ReplaceBoundManagedIndexBufferLocked(self, retainedWrapper);
        retainedWrapper = nullptr;
    }
    if (retainedWrapper != nullptr) {
        retainedWrapper->ReleaseFromBinding();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalGetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9** indexBuffer) {
    if (g_appLocalGetIndices == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (InterlockedCompareExchange(
            &g_appLocalHooksActive, FALSE, FALSE) == FALSE) {
        return g_appLocalGetIndices(self, indexBuffer);
    }
    if (indexBuffer == nullptr) {
        return g_appLocalGetIndices(self, nullptr);
    }

    *indexBuffer = nullptr;
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    IDirect3DIndexBuffer9* inner = nullptr;
    const HRESULT result = g_appLocalGetIndices(self, &inner);
    if (SUCCEEDED(result) && inner != nullptr) {
        ManagedIndexBufferCompat* wrapper =
            RetainManagedIndexBufferByInner(inner);
        if (wrapper != nullptr) {
            inner->Release();
            *indexBuffer = wrapper;
        } else {
            *indexBuffer = inner;
        }
    } else {
        *indexBuffer = inner;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookSetViewport(
    IDirect3DDevice9* self, const D3DVIEWPORT9* viewport) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.setViewport == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (g_internalViewportStateChange) {
        return record.setViewport(self, viewport);
    }
    D3DVIEWPORT9 scaled{};
    return record.setViewport(
        self,
        GetBridge().ScaleStereoViewport(
            self, viewport, scaled)
            ? &scaled
            : viewport);
}

HRESULT STDMETHODCALLTYPE HookSetScissorRect(
    IDirect3DDevice9* self, const RECT* rectangle) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.setScissorRect == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (g_internalViewportStateChange) {
        return record.setScissorRect(self, rectangle);
    }
    RECT scaled{};
    return record.setScissorRect(
        self,
        GetBridge().ScaleStereoScissor(
            self, rectangle, scaled)
            ? &scaled
            : rectangle);
}

HRESULT STDMETHODCALLTYPE HookLateReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    if (g_lateReset == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    ForceHighQualitySource(parameters);
    GetBridge().BeforeReset();
    const HRESULT result = g_lateReset(self, parameters);
    GetBridge().AfterReset(result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookLatePresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion) {
    if (g_latePresent == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    InterlockedIncrement64(&g_presentHookCounts[0]);
    GetBridge().CapturePresent(self);
    return g_latePresent(self, source, destination, overrideWindow,
                         dirtyRegion);
}

HRESULT STDMETHODCALLTYPE HookLateSetViewport(
    IDirect3DDevice9* self, const D3DVIEWPORT9* viewport) {
    if (g_lateSetViewport == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (g_internalViewportStateChange) {
        return g_lateSetViewport(self, viewport);
    }
    D3DVIEWPORT9 scaled{};
    return g_lateSetViewport(
        self,
        GetBridge().ScaleStereoViewport(
            self, viewport, scaled)
            ? &scaled
            : viewport);
}

HRESULT STDMETHODCALLTYPE HookLateSetScissorRect(
    IDirect3DDevice9* self, const RECT* rectangle) {
    if (g_lateSetScissorRect == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (g_internalViewportStateChange) {
        return g_lateSetScissorRect(self, rectangle);
    }
    RECT scaled{};
    return g_lateSetScissorRect(
        self,
        GetBridge().ScaleStereoScissor(
            self, rectangle, scaled)
            ? &scaled
            : rectangle);
}

BOOL CALLBACK InstallLateHooksOnce(PINIT_ONCE, PVOID, PVOID*) {
    auto& bridge = GetBridge();
    using Direct3DCreate9Function = IDirect3D9*(WINAPI*)(UINT);
    const auto createDirect3D =
        ResolveSystemD3D9<Direct3DCreate9Function>("Direct3DCreate9");
    if (createDirect3D == nullptr) {
        bridge.LogHookStatus(
            "ERROR", "late_hook_system_d3d9_failed",
            "Direct3DCreate9 could not be resolved from System32.");
        return TRUE;
    }

    const HWND window = CreateWindowExW(
        0, L"STATIC", L"FearVrD3D9HookProbe", WS_OVERLAPPED,
        0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr),
        nullptr);
    if (window == nullptr) {
        bridge.LogHookStatus(
            "ERROR", "late_hook_window_failed",
            "Win32=" + std::to_string(GetLastError()));
        return TRUE;
    }

    ComPtr<IDirect3D9> direct3D;
    direct3D.Attach(createDirect3D(D3D_SDK_VERSION));
    if (!direct3D) {
        DestroyWindow(window);
        bridge.LogHookStatus(
            "ERROR", "late_hook_d3d9_failed",
            "System Direct3DCreate9 returned null.");
        return TRUE;
    }

    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferWidth = 32;
    parameters.BackBufferHeight = 32;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;

    ComPtr<IDirect3DDevice9> device;
    const HRESULT createResult = direct3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &parameters, device.GetAddressOf());
    if (FAILED(createResult) || !device) {
        DestroyWindow(window);
        std::ostringstream message;
        message << "HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(createResult);
        bridge.LogHookStatus(
            "ERROR", "late_hook_device_failed", message.str());
        return TRUE;
    }

    void** vtable = *reinterpret_cast<void***>(device.Get());
    void* const resetTarget = vtable[16];
    void* const presentTarget = vtable[17];
    void* const setViewportTarget = vtable[47];
    void* const setScissorRectTarget = vtable[75];
    if (resetTarget == reinterpret_cast<void*>(&HookReset) &&
        presentTarget == reinterpret_cast<void*>(&HookPresent) &&
        setViewportTarget ==
            reinterpret_cast<void*>(&HookSetViewport) &&
        setScissorRectTarget ==
            reinterpret_cast<void*>(&HookSetScissorRect)) {
        g_lateHookResult = TRUE;
        bridge.LogHookStatus(
            "INFO", "late_hooks_not_needed",
            "The device vtable is already hooked by the proxy path.");
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    const MH_STATUS initialize = MH_Initialize();
    if (initialize != MH_OK &&
        initialize != MH_ERROR_ALREADY_INITIALIZED) {
        bridge.LogHookStatus(
            "ERROR", "late_hook_initialize_failed",
            MH_StatusToString(initialize));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    MH_STATUS status = MH_CreateHook(
        resetTarget, reinterpret_cast<void*>(&HookLateReset),
        reinterpret_cast<void**>(&g_lateReset));
    if (status != MH_OK) {
        bridge.LogHookStatus(
            "ERROR", "late_hook_reset_create_failed",
            MH_StatusToString(status));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    status = MH_CreateHook(
        presentTarget, reinterpret_cast<void*>(&HookLatePresent),
        reinterpret_cast<void**>(&g_latePresent));
    if (status != MH_OK) {
        MH_RemoveHook(resetTarget);
        g_lateReset = nullptr;
        bridge.LogHookStatus(
            "ERROR", "late_hook_present_create_failed",
            MH_StatusToString(status));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    status = MH_CreateHook(
        setViewportTarget,
        reinterpret_cast<void*>(&HookLateSetViewport),
        reinterpret_cast<void**>(&g_lateSetViewport));
    if (status != MH_OK) {
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resetTarget);
        g_latePresent = nullptr;
        g_lateReset = nullptr;
        bridge.LogHookStatus(
            "ERROR", "late_hook_viewport_create_failed",
            MH_StatusToString(status));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    status = MH_CreateHook(
        setScissorRectTarget,
        reinterpret_cast<void*>(&HookLateSetScissorRect),
        reinterpret_cast<void**>(&g_lateSetScissorRect));
    if (status != MH_OK) {
        MH_RemoveHook(setViewportTarget);
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resetTarget);
        g_lateSetViewport = nullptr;
        g_latePresent = nullptr;
        g_lateReset = nullptr;
        bridge.LogHookStatus(
            "ERROR", "late_hook_scissor_create_failed",
            MH_StatusToString(status));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    status = MH_QueueEnableHook(resetTarget);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(presentTarget);
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(setViewportTarget);
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(setScissorRectTarget);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        MH_RemoveHook(setScissorRectTarget);
        MH_RemoveHook(setViewportTarget);
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resetTarget);
        g_lateSetScissorRect = nullptr;
        g_lateSetViewport = nullptr;
        g_latePresent = nullptr;
        g_lateReset = nullptr;
        bridge.LogHookStatus(
            "ERROR", "late_hook_enable_failed",
            MH_StatusToString(status));
        device.Reset();
        direct3D.Reset();
        DestroyWindow(window);
        return TRUE;
    }

    InterlockedExchange(&g_lateHooksActive, TRUE);
    g_lateHookResult = TRUE;
    bridge.LogHookStatus(
        "INFO", "late_hooks_installed",
        "System D3D9 Reset, Present, viewport, and scissor hooks are "
        "active.");
    device.Reset();
    direct3D.Reset();
    DestroyWindow(window);
    return TRUE;
}

} // namespace

void OnDirect3D9Created(IDirect3D9* direct3D) noexcept {
    PatchD3D9(direct3D, false);
}

bool OnDirect3D9ExCreated(IDirect3D9Ex* direct3D) noexcept {
    return PatchD3D9(direct3D, true);
}

void ApplyEngineFixes() noexcept {
    GetBridge().ApplyEngineFixes();
}

BOOL InstallLateD3D9Hooks() noexcept {
    if (!InitOnceExecuteOnce(
            &g_lateHookOnce, InstallLateHooksOnce, nullptr, nullptr)) {
        return FALSE;
    }
    return g_lateHookResult;
}

BOOL AreLateD3D9HooksActive() noexcept {
    return InterlockedCompareExchange(
        &g_lateHooksActive, FALSE, FALSE) != FALSE;
}

std::uint64_t PresentHookCount(std::uint32_t kind) noexcept {
    if (kind >= 3) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        InterlockedCompareExchange64(
            &g_presentHookCounts[kind], 0, 0));
}

std::uint32_t ManagedIndexBindingCount() noexcept {
    std::lock_guard<std::recursive_mutex> bindingLock(
        g_managedIndexBindingCallMutex);
    std::uint32_t count = 0;
    AcquireSRWLockShared(&g_boundManagedIndexLock);
    for (const BoundManagedIndexBuffer& record :
         g_boundManagedIndexBuffers) {
        if (record.device != nullptr && record.buffer != nullptr) {
            ++count;
        }
    }
    ReleaseSRWLockShared(&g_boundManagedIndexLock);
    return count;
}

BOOL IsHostConnected() noexcept {
    return GetBridge().IsConnected();
}

BOOL IsStereoAvailable() noexcept {
    return GetBridge().StereoAvailable();
}

BOOL IsStereoEnabled() noexcept {
    return GetBridge().StereoEnabled();
}

void SetStereoEnabled(BOOL enabled) noexcept {
    GetBridge().SetStereoEnabled(enabled);
}

void SetFovScalePercent(std::uint32_t percent) noexcept {
    GetBridge().SetFovScalePercent(percent);
}

BOOL IsTranslationEnabled() noexcept {
    return GetBridge().TranslationEnabled();
}

void SetTranslationEnabled(BOOL enabled) noexcept {
    GetBridge().SetTranslationEnabled(enabled);
}

BOOL IsStereoHudEnabled() noexcept {
    return GetBridge().StereoHudEnabled();
}

void SetStereoHudEnabled(BOOL enabled) noexcept {
    GetBridge().SetStereoHudEnabled(enabled);
}

BOOL IsComfortModeEnabled() noexcept {
    return GetBridge().ComfortModeEnabled();
}

void SetComfortModeEnabled(BOOL enabled) noexcept {
    GetBridge().SetComfortModeEnabled(enabled);
}

void SetMenuActive(BOOL active) noexcept {
    GetBridge().SetMenuActive(active);
}

void RequestRecenter() noexcept {
    GetBridge().RequestRecenter();
}

BOOL IsFlatPanelActive() noexcept {
    return GetBridge().FlatPanelActive();
}

void RegisterStereoToggleCallback(
    StereoToggleCallback callback) noexcept {
    GetBridge().RegisterStereoToggle(callback);
}

BOOL GetRenderRequest(FearVrRenderRequest* request) noexcept {
    return GetBridge().ReadRenderRequest(request);
}

BOOL WaitForNewRenderRequest(
    std::uint64_t previousFrameId,
    std::uint32_t timeoutMilliseconds,
    FearVrRenderRequest* request) noexcept {
    return GetBridge().WaitForNewRenderRequest(
        previousFrameId, timeoutMilliseconds, request);
}

BOOL GetInputState(FearVrInputState* input) noexcept {
    return GetBridge().ReadInputState(input);
}

BOOL SubmitHapticRequest(
    const FearVrHapticRequest* request) noexcept {
    return GetBridge().WriteHapticRequest(request);
}

void BeginEye(std::uint32_t eye) noexcept {
    GetBridge().BeginStereoEye(eye);
}

void CaptureEye(std::uint32_t eye) noexcept {
    GetBridge().CaptureStereoEye(eye);
}

void EndStereoFrame(std::uint64_t frameId) noexcept {
    GetBridge().EndStereoFrame(frameId);
}

void ReportHookStatus(const char* level, const char* event,
                      const char* message) noexcept {
    GetBridge().LogHookStatus(
        level == nullptr ? "INFO" : level,
        event == nullptr ? "stereo_hook" : event,
        message == nullptr ? "" : message);
}

} // namespace fearvr
