#include "bridge.h"

#include <array>
#include <chrono>
#include <cstdint>
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
#include "stereo_hud_math.h"
#include "system_d3d9.h"

namespace fearvr {
namespace {

using Microsoft::WRL::ComPtr;

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
    // Opt-in: veraendert genau ein Present-Bild fuer die Oberflaechenprobe.
    // Ohne diesen Schalter wird kein Spielbild veraendert.
    bool magentaSurfaceTest{false};
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
                       L"-fearvr-magenta-surface-test") == 0) {
            config.magentaSurfaceTest = true;
        }
    }
    LocalFree(arguments);
    return config;
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

enum class TransferMode {
    None,
    DirectShared,
    CpuViaD3D9Ex
};

UINT SurfaceBytesPerPixel(D3DFORMAT format) noexcept {
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:
        return 4;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
        return 2;
    case D3DFMT_A8:
        return 1;
    default:
        return 0;
    }
}

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

void ReleaseTrackedRenderTargets() noexcept;

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
    }

    void LogHookStatus(const char* level, const char* event,
                       const std::string& message) noexcept {
        logger_.Write(level, event, message);
    }

    bool ShouldDeferEndSceneCapture() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stereoAccepting_ || stereoFrameReady_;
    }
    bool ProbeDiagnosticSurface(
        IDirect3DDevice9* device, IDirect3DSurface9* surface,
        const char* name) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return ProbeD3D9Surface(
            device, surface, name, 0, 0);
    }


    void CapturePresent(
        IDirect3DDevice9* device,
        IDirect3DSurface9* presentedSurface,
        IDirect3DSwapChain9* presentingSwapChain,
        HWND destinationWindow,
        const char* presentationPath) noexcept {
        if (device == nullptr || presentedSurface == nullptr ||
            config_.sessionId == 0) {
            return;
        }
        PollStereoToggle();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureIpc()) {
            return;
        }

        InterlockedIncrement64(Atomic64(shared_->gameHeartbeat));
        PollPending();
        UpdateHostConnection();
        EnsureDeviceMetadata(device);
        UpdateAdapterMatch();
        LogPresentSource(
            device, presentedSurface, presentingSwapChain,
            destinationWindow, presentationPath);

        if (!hostConnected_ ||
            (shared_->bridgeFlags & FEARVR_BF_ADAPTER_MATCH) == 0) {
            return;
        }

        ComPtr<IDirect3DDevice9> surfaceDevice;
        HRESULT result = presentedSurface->GetDevice(
            surfaceDevice.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult("present_surface_get_device_failed", result);
            return;
        }
        if (surfaceDevice.Get() != device) {
            logger_.Write(
                "ERROR", "present_surface_device_mismatch",
                "The presenting surface belongs to a different D3D9 device.");
            return;
        }

        IDirect3DSurface9* captureSource = presentedSurface;
        D3DSURFACE_DESC captureDescription{};
        result = captureSource->GetDesc(&captureDescription);

        if (FAILED(result)) {
            LogHresult("present_surface_desc_failed", result);
            return;
        }

        ComPtr<IDirect3DSurface9> currentRenderTarget;
        const HRESULT currentTargetResult = device->GetRenderTarget(
            0, currentRenderTarget.ReleaseAndGetAddressOf());
        if (FAILED(currentTargetResult)) {
            LogHresult(
                "present_current_render_target_failed",
                currentTargetResult);
        }
        if (currentRenderTarget &&
            preferredCaptureSource_.Get() == currentRenderTarget.Get()) {
            D3DSURFACE_DESC preferredDescription{};
            const HRESULT preferredResult =
                currentRenderTarget->GetDesc(&preferredDescription);
            if (SUCCEEDED(preferredResult) &&
                preferredDescription.Width == captureDescription.Width &&
                preferredDescription.Height == captureDescription.Height &&
                preferredDescription.Format == captureDescription.Format) {
                captureSource = currentRenderTarget.Get();
                captureDescription = preferredDescription;
            } else if (FAILED(preferredResult)) {
                LogHresult(
                    "preferred_capture_source_desc_failed",
                    preferredResult);
            }
        }
        if (!EnsureResources(
                device, captureDescription.Width,
                captureDescription.Height, captureDescription.Format,
                captureDescription.MultiSampleType)) {
            return;
        }
        if (pending_.active) {
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

        const bool stereo =
            stereoFrameReady_ &&
            stereoEyeCaptured_[FEARVR_EYE_LEFT] &&
            stereoEyeCaptured_[FEARVR_EYE_RIGHT];
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

        if (!directProbeNonBlack_ &&
            directProbeAttempts_ < 64 &&
            (directProbeAttempts_ == 0 ||
             generation >=
                 static_cast<std::uint64_t>(directProbeAttempts_) * 30U)) {
            ++directProbeAttempts_;
            bool presentedProbeCompleted = false;
            const bool presentedHasPixels = ProbeD3D9Surface(
                device, presentedSurface, "presented_backbuffer",
                frameId, generation, &presentedProbeCompleted);
            bool currentHasPixels = false;
            bool currentProbeCompleted = false;
            if (currentRenderTarget &&
                currentRenderTarget.Get() != presentedSurface) {
                currentHasPixels = ProbeD3D9Surface(
                    device, currentRenderTarget.Get(),
                    "current_render_target", frameId, generation,
                    &currentProbeCompleted);
            }
            directProbeNonBlack_ =
                (presentedProbeCompleted && presentedHasPixels) ||
                (currentProbeCompleted && currentHasPixels);
            if (presentedProbeCompleted && !presentedHasPixels &&
                currentProbeCompleted && currentHasPixels) {
                D3DSURFACE_DESC currentDescription{};
                const HRESULT descriptionResult =
                    currentRenderTarget->GetDesc(&currentDescription);
                if (SUCCEEDED(descriptionResult) &&
                    currentDescription.Width == captureDescription.Width &&
                    currentDescription.Height == captureDescription.Height &&
                    currentDescription.Format == captureDescription.Format) {
                    preferredCaptureSource_ = currentRenderTarget;
                    captureSource = currentRenderTarget.Get();
                    captureDescription = currentDescription;
                    logger_.Write(
                        "WARN", "capture_source_fallback",
                        "The presenting backbuffer was black while render "
                        "target 0 contained pixels; capture now follows the "
                        "active final render target.");
                } else if (FAILED(descriptionResult)) {
                    LogHresult(
                        "fallback_capture_source_desc_failed",
                        descriptionResult);
                }
            }
        }

        if (config_.magentaSurfaceTest && !magentaTestDone_) {
            magentaTestDone_ = true;
            const HRESULT fillResult = device->ColorFill(
                presentedSurface, nullptr,
                D3DCOLOR_XRGB(255, 0, 255));
            if (FAILED(fillResult)) {
                LogHresult("magenta_surface_test_failed", fillResult);
            } else {
                logger_.Write(
                    "WARN", "magenta_surface_test",
                    "The exact surface passed to the next Present was "
                    "filled magenta for one frame.");
                ProbeD3D9Surface(
                    device, presentedSurface, "presented_magenta",
                    frameId, generation);
            }
        }

        ComPtr<IDirect3DSurface9> resolvedCaptureSource;
        if (!ResolveCaptureSource(
                device, captureSource,
                resolvedCaptureSource.ReleaseAndGetAddressOf())) {
            ReleaseClaimedPair(slotIndex);
            return;
        }
        captureSource = resolvedCaptureSource.Get();
        bool copied = false;
        stereoHudFlatFrame_ = false;
        if (stereo) {
            copied = transferMode_ == TransferMode::CpuViaD3D9Ex
                ? CopyStereoFrameViaCpu(
                      device, captureSource, slotIndex)
                : CopyStereoFrameDirect(device, slotIndex);
        } else {
            copied = transferMode_ == TransferMode::CpuViaD3D9Ex
                ? CopyFrameViaCpu(device, captureSource, slotIndex)
                : CopyFrameDirect(device, captureSource, slotIndex);
        }

        if (!copied) {
            ReleaseClaimedPair(slotIndex);
            if (stereo) {
                ClearStereoFrame();
            }
            return;
        }
        if (stereo) {
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

        pending_.active = true;
        pending_.slotIndex = slotIndex;
        pending_.frameId = frameId;
        pending_.generation = generation;

        const ULONGLONG deadline = GetTickCount64() + 3;
        do {
            if (PollPending()) {
                break;
            }
            SwitchToThread();
        } while (GetTickCount64() < deadline);
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
        if (eye == FEARVR_EYE_LEFT) {
            if (stereoFrameReady_) {
                stereoAccepting_ = false;
                return;
            }
            stereoEyeCaptured_.fill(false);
            stereoFrameId_ = 0;
            stereoAccepting_ = true;
        }
    }

    void CaptureStereoEye(std::uint32_t eye) noexcept {
        if (eye >= FEARVR_EYE_COUNT || config_.sessionId == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stereoAccepting_ || device_ == nullptr ||
            !resourcesReady_ || !stereoCapture_[eye]) {
            return;
        }

        ComPtr<IDirect3DSurface9> eyeSource;
        HRESULT result = device_->GetRenderTarget(
            0, eyeSource.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            LogHresult("stereo_get_render_target_failed", result);
            ComPtr<IDirect3DSwapChain9> swapChain;
            result = device_->GetSwapChain(
                0, swapChain.ReleaseAndGetAddressOf());
            if (FAILED(result) || !swapChain) {
                LogHresult("stereo_get_swapchain_failed", result);
                return;
            }
            result = swapChain->GetBackBuffer(
                0, D3DBACKBUFFER_TYPE_MONO,
                eyeSource.ReleaseAndGetAddressOf());
            if (FAILED(result)) {
                LogHresult(
                    "stereo_get_presented_surface_failed", result);
                return;
            }
        }
        D3DSURFACE_DESC description{};
        result = eyeSource->GetDesc(&description);
        if (FAILED(result)) {
            LogHresult("stereo_source_desc_failed", result);
            return;
        }
        if (description.Width != width_ ||
            description.Height != height_) {
            logger_.Write(
                "WARN", "stereo_capture_size_changed",
                "Stereo capture deferred until resources are recreated.");
            return;
        }
        ComPtr<IDirect3DSurface9> resolvedEyeSource;
        if (!ResolveCaptureSource(
                device_, eyeSource.Get(),
                resolvedEyeSource.ReleaseAndGetAddressOf())) {
            return;
        }
        result = device_->StretchRect(
            resolvedEyeSource.Get(), nullptr,
            stereoCapture_[eye].Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("stereo_stage_copy_failed", result);
            return;
        }
        stereoEyeCaptured_[eye] = true;
    }

    void EndStereoFrame(std::uint64_t frameId) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
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
        frameReadyEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, frameReadyName.c_str());
        slotConsumedEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, consumedName.c_str());
        if (frameReadyEvent_ == nullptr || slotConsumedEvent_ == nullptr) {
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
                         UINT height, D3DFORMAT sourceFormat,
                         D3DMULTISAMPLE_TYPE sourceMultisample) noexcept {
        if (resourcesReady_ && width_ == width && height_ == height &&
            sourceFormat_ == sourceFormat &&
            sourceMultisample_ == sourceMultisample && device_ == device) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < nextResourceRetryTick_) {
            return false;
        }

        ReleaseResources();
        device_ = device;
        width_ = width;
        height_ = height;
        sourceFormat_ = sourceFormat;
        sourceMultisample_ = sourceMultisample;

        ComPtr<IDirect3DDevice9Ex> deviceEx;
        bool created = false;
        if (SUCCEEDED(device->QueryInterface(
                IID_PPV_ARGS(deviceEx.ReleaseAndGetAddressOf())))) {
            created = CreateSharedSlots(device, width, height);
            if (created) {
                transferMode_ = TransferMode::DirectShared;
                HRESULT resolveResult = device->CreateTexture(
                    width, height, 1, D3DUSAGE_RENDERTARGET,
                    sourceFormat, D3DPOOL_DEFAULT,
                    directResolveTexture_.ReleaseAndGetAddressOf(),
                    nullptr);
                if (SUCCEEDED(resolveResult)) {
                    resolveResult =
                        directResolveTexture_->GetSurfaceLevel(
                            0,
                            directResolveSurface_
                                .ReleaseAndGetAddressOf());
                }
                if (FAILED(resolveResult)) {
                    LogHresult("direct_resolve_create_failed",
                               resolveResult);
                    created = false;
                } else {
                    logger_.Write(
                        "INFO", "direct_resolve_ready",
                        "Single-sample intermediate handles MSAA "
                        "backbuffers before shared-texture copies.");
                }
            }
        } else {
            created = CreateCpuInteropResources(device, width, height);
        }
        if (created && sourceMultisample != D3DMULTISAMPLE_NONE &&
            !directResolveSurface_) {
            HRESULT resolveResult = device->CreateTexture(
                width, height, 1, D3DUSAGE_RENDERTARGET,
                sourceFormat, D3DPOOL_DEFAULT,
                directResolveTexture_.ReleaseAndGetAddressOf(), nullptr);
            if (SUCCEEDED(resolveResult)) {
                resolveResult = directResolveTexture_->GetSurfaceLevel(
                    0, directResolveSurface_.ReleaseAndGetAddressOf());
            }
            if (FAILED(resolveResult)) {
                LogHresult("cpu_resolve_create_failed", resolveResult);
                created = false;
            } else {
                logger_.Write(
                    "INFO", "cpu_resolve_ready",
                    "The CPU transfer path has a matching single-sample "
                    "MSAA resolve target.");
            }
        }
        if (created) {
            created =
                CreateStereoCaptureSurfaces(device, width, height);
        }
        if (created && config_.stereoHudEnabled &&
            !config_.disableGpuHud) {
            // Scheitert der Kompositor, bleibt das HUD trotzdem: dann mischt
            // wieder die CPU. Kein Grund, den ganzen Bildpfad aufzugeben.
            hudCompositor_.Initialize(device, width, height, logger_);
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
        message << "size=" << width << 'x' << height
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

    bool ResolveCaptureSource(
        IDirect3DDevice9* device, IDirect3DSurface9* source,
        IDirect3DSurface9** output) noexcept {
        if (device == nullptr || source == nullptr || output == nullptr) {
            return false;
        }
        *output = nullptr;
        D3DSURFACE_DESC sourceDescription{};
        HRESULT result = source->GetDesc(&sourceDescription);
        if (FAILED(result)) {
            LogHresult("capture_source_desc_failed", result);
            return false;
        }
        if (sourceDescription.MultiSampleType == D3DMULTISAMPLE_NONE) {
            source->AddRef();
            *output = source;
            return true;
        }
        if (!directResolveSurface_) {
            logger_.Write(
                "ERROR", "capture_resolve_missing",
                "A multisampled source has no resolve surface.");
            return false;
        }
        D3DSURFACE_DESC resolveDescription{};
        result = directResolveSurface_->GetDesc(&resolveDescription);
        if (FAILED(result)) {
            LogHresult("capture_resolve_desc_failed", result);
            return false;
        }
        if (resolveDescription.Width != sourceDescription.Width ||
            resolveDescription.Height != sourceDescription.Height ||
            resolveDescription.Format != sourceDescription.Format ||
            resolveDescription.MultiSampleType != D3DMULTISAMPLE_NONE) {
            logger_.Write(
                "ERROR", "capture_resolve_mismatch",
                "MSAA source and resolve target dimensions or format differ.");
            return false;
        }
        result = device->StretchRect(
            source, nullptr, directResolveSurface_.Get(), nullptr,
            D3DTEXF_NONE);
        if (FAILED(result)) {
            LogHresult("capture_msaa_resolve_failed", result);
            return false;
        }
        directResolveSurface_->AddRef();
        *output = directResolveSurface_.Get();
        if (!resolvedProbeDone_) {
            resolvedProbeDone_ = true;
            ProbeD3D9Surface(
                device, directResolveSurface_.Get(), "resolved_intermediate",
                frameId_, generation_);
        }
        return true;
    }

    bool CopyFrameDirect(IDirect3DDevice9* device,
                         IDirect3DSurface9* backBuffer,
                         std::uint32_t slotIndex) noexcept {
        IDirect3DSurface9* source = backBuffer;
        D3DSURFACE_DESC sourceDescription{};
        HRESULT result = backBuffer->GetDesc(&sourceDescription);
        if (FAILED(result)) {
            LogHresult("direct_source_desc_failed", result);
            return false;
        }
        if (sourceDescription.MultiSampleType !=
            D3DMULTISAMPLE_NONE) {
            result = device->StretchRect(
                backBuffer, nullptr, directResolveSurface_.Get(),
                nullptr, D3DTEXF_NONE);
            if (FAILED(result)) {
                LogHresult("direct_msaa_resolve_failed", result);
                return false;
            }
            source = directResolveSurface_.Get();
        }

        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            SlotResource& resource = resources_[eye][slotIndex];
            result = device->StretchRect(
                source, nullptr, resource.surface.Get(), nullptr,
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
        ReleaseTrackedRenderTargets();
        pending_ = {};
        resourcesReady_ = false;
        transferMode_ = TransferMode::None;
        sourceFormat_ = D3DFMT_UNKNOWN;
        sourceMultisample_ = D3DMULTISAMPLE_NONE;
        resolvedProbeDone_ = false;
        preferredCaptureSource_.Reset();
        // Zuerst der Kompositor: Er hält Render-Targets auf demselben Gerät.
        hudCompositor_.Release();
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
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
        bridgeUpload_.Reset();
        presentedReadback_.Reset();
        rightWorldReadback_.Reset();
        gameReadback_.Reset();
        directProbeReadback_.Reset();
        directResolveSurface_.Reset();
        directResolveTexture_.Reset();
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

    bool PollPending() noexcept {
        if (!pending_.active) {
            return true;
        }
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            SlotResource& resource =
                resources_[eye][pending_.slotIndex];
            if (!resource.completion ||
                resource.completion->GetData(
                    nullptr, 0, D3DGETDATA_FLUSH) != S_OK) {
                return false;
            }
        }
        if (transferMode_ == TransferMode::DirectShared &&
            directProbeGeneration_ == pending_.generation) {
            ProbeD3D9Surface(
                device_,
                resources_[FEARVR_EYE_LEFT][pending_.slotIndex].surface.Get(),
                "shared", pending_.frameId, pending_.generation);
            directProbeGeneration_ = 0;
        }

        MemoryBarrier();
        for (std::uint32_t eye = 0; eye < FEARVR_EYE_COUNT; ++eye) {
            InterlockedExchange(
                AtomicState(shared_->slot[eye][pending_.slotIndex]),
                FEARVR_SLOT_READY);
        }
        SetEvent(frameReadyEvent_);
        if (pending_.frameId == 1 ||
            pending_.frameId % 300 == 0) {
            std::ostringstream message;
            message << "frame=" << pending_.frameId
                    << " generation=" << pending_.generation
                    << " slot=" << pending_.slotIndex;
            logger_.Write("INFO", "frame_ready", message.str());
        }
        pending_ = {};
        return true;
    }

    void LogPresentSource(
        IDirect3DDevice9* device, IDirect3DSurface9* surface,
        IDirect3DSwapChain9* swapChain, HWND destinationWindow,
        const char* path) noexcept {
        void* const identity = swapChain != nullptr
            ? static_cast<void*>(swapChain)
            : static_cast<void*>(surface);
        for (std::size_t index = 0; index < loggedPresentSourceCount_;
             ++index) {
            if (loggedPresentSources_[index] == identity) {
                return;
            }
        }
        if (loggedPresentSourceCount_ < loggedPresentSources_.size()) {
            loggedPresentSources_[loggedPresentSourceCount_++] = identity;
        }

        D3DSURFACE_DESC description{};
        const HRESULT descriptionResult = surface->GetDesc(&description);
        D3DPRESENT_PARAMETERS parameters{};
        const HRESULT parametersResult = swapChain != nullptr
            ? swapChain->GetPresentParameters(&parameters)
            : D3DERR_INVALIDCALL;
        if (destinationWindow == nullptr && SUCCEEDED(parametersResult)) {
            destinationWindow = parameters.hDeviceWindow;
        }
        if (destinationWindow == nullptr) {
            D3DDEVICE_CREATION_PARAMETERS creation{};
            const HRESULT creationResult =
                device->GetCreationParameters(&creation);
            if (SUCCEEDED(creationResult)) {
                destinationWindow = creation.hFocusWindow;
            } else {
                LogHresult("present_device_creation_params_failed",
                           creationResult);
            }
        }
        char title[256]{};
        if (destinationWindow != nullptr) {
            GetWindowTextA(destinationWindow, title,
                           static_cast<int>(sizeof(title)));
        }

        std::ostringstream message;
        message << "path=" << (path == nullptr ? "unknown" : path)
                << " swapchain=0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(swapChain)
                << " device=0x"
                << reinterpret_cast<std::uintptr_t>(device)
                << " backbuffer=0x"
                << reinterpret_cast<std::uintptr_t>(surface)
                << " hwnd=0x"
                << reinterpret_cast<std::uintptr_t>(destinationWindow)
                << " desc_hr=0x"
                << static_cast<std::uint32_t>(descriptionResult)
                << " present_params_hr=0x"
                << static_cast<std::uint32_t>(parametersResult)
                << std::dec << " title=" << title
                << " size=" << description.Width << 'x'
                << description.Height << " format="
                << static_cast<unsigned>(description.Format)
                << " msaa="
                << static_cast<unsigned>(description.MultiSampleType)
                << " swap_effect="
                << static_cast<unsigned>(parameters.SwapEffect);
        logger_.Write(
            SUCCEEDED(descriptionResult) ? "INFO" : "ERROR",
            "d3d9_present_source", message.str());
    }

    bool ProbeD3D9Surface(IDirect3DDevice9* device,
                          IDirect3DSurface9* surface,
                          const char* surfaceName,
                          std::uint64_t frameId,
                          std::uint64_t generation,
                          bool* probeCompleted = nullptr) noexcept {
        if (probeCompleted != nullptr) {
            *probeCompleted = false;
        }
        if (device == nullptr || surface == nullptr) {
            return false;
        }

        D3DSURFACE_DESC sourceDescription{};
        HRESULT result = surface->GetDesc(&sourceDescription);
        if (FAILED(result)) {
            LogHresult("d3d9_pixel_probe_desc_failed", result);
            return false;
        }
        if (sourceDescription.MultiSampleType !=
            D3DMULTISAMPLE_NONE) {
            if (!directResolveSurface_) {
                logger_.Write(
                    "ERROR", "d3d9_pixel_probe_resolve_missing",
                    "MSAA surface has no single-sample resolve target.");
                return false;
            }
            result = device->StretchRect(
                surface, nullptr, directResolveSurface_.Get(),
                nullptr, D3DTEXF_NONE);
            if (FAILED(result)) {
                LogHresult("d3d9_pixel_probe_resolve_failed", result);
                return false;
            }
            surface = directResolveSurface_.Get();
            result = surface->GetDesc(&sourceDescription);
            if (FAILED(result)) {
                LogHresult("d3d9_pixel_probe_resolve_desc_failed",
                           result);
                return false;
            }
        }
        if (sourceDescription.MultiSampleType !=
            D3DMULTISAMPLE_NONE) {
            logger_.Write(
                "ERROR", "d3d9_pixel_probe_source_still_msaa",
                "GetRenderTargetData requires a single-sample source.");
            return false;
        }
        D3DSURFACE_DESC readbackDescription{};
        HRESULT readbackDescriptionResult = directProbeReadback_
            ? directProbeReadback_->GetDesc(&readbackDescription)
            : D3DERR_INVALIDCALL;
        if (directProbeReadback_ &&
            FAILED(readbackDescriptionResult)) {
            LogHresult(
                "d3d9_pixel_probe_readback_desc_failed",
                readbackDescriptionResult);
        }
        bool readbackMatches =
            directProbeReadback_ &&
            SUCCEEDED(readbackDescriptionResult) &&
            readbackDescription.Width == sourceDescription.Width &&
            readbackDescription.Height == sourceDescription.Height &&
            readbackDescription.Format == sourceDescription.Format &&
            readbackDescription.Pool == D3DPOOL_SYSTEMMEM;
        if (!readbackMatches) {
            directProbeReadback_.Reset();
            result = device->CreateOffscreenPlainSurface(
                sourceDescription.Width, sourceDescription.Height,
                sourceDescription.Format, D3DPOOL_SYSTEMMEM,
                directProbeReadback_.ReleaseAndGetAddressOf(),
                nullptr);
            if (FAILED(result)) {
                LogHresult("d3d9_pixel_probe_create_failed", result);
                return false;
            }
            readbackDescriptionResult =
                directProbeReadback_->GetDesc(&readbackDescription);
            if (FAILED(readbackDescriptionResult)) {
                LogHresult(
                    "d3d9_pixel_probe_created_desc_failed",
                    readbackDescriptionResult);
                return false;
            }
            readbackMatches =
                readbackDescription.Width == sourceDescription.Width &&
                readbackDescription.Height == sourceDescription.Height &&
                readbackDescription.Format == sourceDescription.Format &&
                readbackDescription.Pool == D3DPOOL_SYSTEMMEM;
            if (!readbackMatches) {
                logger_.Write(
                    "ERROR", "d3d9_pixel_probe_readback_mismatch",
                    "The system-memory readback does not match the source "
                    "dimensions, format, and pool.");
                return false;
            }
        }

        const UINT bytesPerPixel =
            SurfaceBytesPerPixel(sourceDescription.Format);
        if (bytesPerPixel == 0) {
            logger_.Write(
                "ERROR", "d3d9_pixel_probe_format_unsupported",
                "The readback sentinel cannot size this pixel format.");
            return false;
        }
        const std::size_t rowBytes =
            static_cast<std::size_t>(sourceDescription.Width) * bytesPerPixel;
        D3DLOCKED_RECT sentinel{};
        result = directProbeReadback_->LockRect(&sentinel, nullptr, 0);
        if (FAILED(result)) {
            LogHresult("d3d9_pixel_probe_sentinel_lock_failed", result);
            return false;
        }
        if (sentinel.Pitch < 0 ||
            static_cast<std::size_t>(sentinel.Pitch) < rowBytes) {
            directProbeReadback_->UnlockRect();
            logger_.Write(
                "ERROR", "d3d9_pixel_probe_pitch_invalid",
                "The system-memory readback pitch is smaller than one row.");
            return false;
        }
        auto* sentinelRow = static_cast<std::uint8_t*>(sentinel.pBits);
        for (UINT row = 0; row < sourceDescription.Height; ++row) {
            std::memset(sentinelRow, 0xCD, rowBytes);
            sentinelRow += sentinel.Pitch;
        }
        result = directProbeReadback_->UnlockRect();
        if (FAILED(result)) {
            LogHresult("d3d9_pixel_probe_sentinel_unlock_failed", result);
            return false;
        }

        const HRESULT copyResult = device->GetRenderTargetData(
            surface, directProbeReadback_.Get());
        if (FAILED(copyResult)) {
            std::ostringstream message;
            message << "surface=" << surfaceName
                    << " HRESULT=0x" << std::hex << std::uppercase
                    << static_cast<std::uint32_t>(copyResult)
                    << " format=" << std::dec
                    << static_cast<unsigned>(sourceDescription.Format)
                    << " multisample="
                    << static_cast<unsigned>(
                           sourceDescription.MultiSampleType);
            logger_.Write("ERROR", "d3d9_pixel_probe_copy_failed",
                          message.str());
            return false;
        }

        D3DLOCKED_RECT mapped{};
        const HRESULT lockResult = directProbeReadback_->LockRect(
            &mapped, nullptr, D3DLOCK_READONLY);
        if (FAILED(lockResult)) {
            LogHresult("d3d9_pixel_probe_lock_failed", lockResult);
            return false;
        }

        std::uint64_t totalBytes = 0;
        std::uint64_t sentinelBytes = 0;
        std::uint64_t zeroBytes = 0;
        for (UINT y = 0; y < sourceDescription.Height; ++y) {
            const auto* row =
                static_cast<const std::uint8_t*>(mapped.pBits) +
                static_cast<std::size_t>(y) * mapped.Pitch;
            for (std::size_t offset = 0; offset < rowBytes; ++offset) {
                sentinelBytes += row[offset] == 0xCD ? 1U : 0U;
                zeroBytes += row[offset] == 0 ? 1U : 0U;
                ++totalBytes;
            }
        }
        if (totalBytes != 0 && sentinelBytes == totalBytes) {
            const HRESULT unlockResult =
                directProbeReadback_->UnlockRect();
            if (FAILED(unlockResult)) {
                LogHresult(
                    "d3d9_pixel_probe_unlock_failed", unlockResult);
            }
            logger_.Write(
                "ERROR", "d3d9_pixel_probe_sentinel_unchanged",
                "GetRenderTargetData reported success but left every byte "
                "at the 0xCD sentinel value.");
            return false;
        }
        const char* const readbackState =
            totalBytes != 0 && zeroBytes == totalBytes
                ? "all_zero"
                : "mixed";

        const UINT stepX =
            (std::max)(1U, sourceDescription.Width / 64U);
        const UINT stepY =
            (std::max)(1U, sourceDescription.Height / 64U);
        std::uint64_t samples = 0;
        std::uint64_t nonBlackSamples = 0;
        std::uint64_t blue = 0;
        std::uint64_t green = 0;
        std::uint64_t red = 0;
        for (UINT y = 0; y < sourceDescription.Height;
             y += stepY) {
            const auto* row =
                static_cast<const std::uint8_t*>(mapped.pBits) +
                static_cast<std::size_t>(y) * mapped.Pitch;
            for (UINT x = 0; x < sourceDescription.Width;
                x += stepX) {
                const auto* pixel =
                    row + static_cast<std::size_t>(x) * bytesPerPixel;
                blue += pixel[0];
                green += bytesPerPixel > 1 ? pixel[1] : 0;
                red += bytesPerPixel > 2 ? pixel[2] : 0;
                bool nonBlack = false;
                const UINT colorBytes =
                    bytesPerPixel == 4 ? 3U : bytesPerPixel;
                for (UINT channel = 0; channel < colorBytes;
                     ++channel) {
                    nonBlack = nonBlack || pixel[channel] != 0;
                }
                nonBlackSamples += nonBlack ? 1U : 0U;
                ++samples;
            }
        }
        const HRESULT unlockResult = directProbeReadback_->UnlockRect();
        if (FAILED(unlockResult)) {
            LogHresult("d3d9_pixel_probe_unlock_failed", unlockResult);
            return false;
        }

        std::ostringstream message;
        message << "surface=" << surfaceName
                << " frame=" << frameId
                << " generation=" << generation
                << " attempt=" << directProbeAttempts_
                << " samples=" << samples
                << " nonzero_samples=" << nonBlackSamples
                << " readback_state=" << readbackState
                << " sentinel_bytes=" << sentinelBytes
                << " avg_bgr=";
        if (samples == 0) {
            message << "0,0,0";
        } else {
            message << blue / samples << ','
                    << green / samples << ','
                    << red / samples;
        }
        logger_.Write(nonBlackSamples == 0 ? "WARN" : "INFO",
                      "d3d9_pixel_probe", message.str());
        if (probeCompleted != nullptr) {
            *probeCompleted = true;
        }
        return nonBlackSamples != 0;
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
    FearVrSharedHeader* shared_{nullptr};
    IDirect3DDevice9* device_{nullptr};
    ComPtr<IDirect3D9Ex> bridgeDirect3DEx_;
    ComPtr<IDirect3DDevice9Ex> bridgeDeviceEx_;
    ComPtr<IDirect3DTexture9> gameCaptureTexture_;
    ComPtr<IDirect3DSurface9> gameCapture_;
    ComPtr<IDirect3DSurface9> gameReadback_;
    ComPtr<IDirect3DSurface9> rightWorldReadback_;
    ComPtr<IDirect3DSurface9> presentedReadback_;
    ComPtr<IDirect3DSurface9> directProbeReadback_;
    ComPtr<IDirect3DTexture9> directResolveTexture_;
    ComPtr<IDirect3DSurface9> directResolveSurface_;
    ComPtr<IDirect3DSurface9> preferredCaptureSource_;
    ComPtr<IDirect3DSurface9> bridgeUpload_;
    std::array<ComPtr<IDirect3DTexture9>, FEARVR_EYE_COUNT>
        stereoCaptureTexture_{};
    std::array<ComPtr<IDirect3DSurface9>, FEARVR_EYE_COUNT>
        stereoCapture_{};
    GpuHudCompositor hudCompositor_;
    std::array<std::array<SlotResource, FEARVR_SLOTS_PER_EYE>,
               FEARVR_EYE_COUNT>
        resources_{};
    PendingFrame pending_{};
    UINT width_{0};
    UINT height_{0};
    D3DFORMAT sourceFormat_{D3DFMT_UNKNOWN};
    D3DMULTISAMPLE_TYPE sourceMultisample_{D3DMULTISAMPLE_NONE};
    std::array<void*, 32> loggedPresentSources_{};
    std::size_t loggedPresentSourceCount_{0};
    std::uint32_t nextSlot_{0};
    std::uint64_t frameId_{0};
    std::uint64_t generation_{0};
    std::uint64_t droppedFrames_{0};
    std::uint64_t stereoFrameId_{0};
    std::uint64_t stereoFrames_{0};
    std::uint64_t stereoHudFrames_{0};
    std::uint64_t gameAdapterLuid_{0};
    std::uint64_t lastHostHeartbeat_{0};
    ULONGLONG lastHostHeartbeatTick_{0};
    ULONGLONG nextResourceRetryTick_{0};
    HWND companionWindow_{nullptr};
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
    bool stereoKeyWasDown_{false};
    bool recenterKeyWasDown_{false};
    bool comfortKeyWasDown_{false};
    std::uint32_t directProbeAttempts_{0};
    bool directProbeNonBlack_{false};
    std::uint64_t directProbeGeneration_{0};
    bool resolvedProbeDone_{false};
    bool magentaTestDone_{false};
    bool comfortModeEnabled_{false};
    bool menuActive_{false};
    std::uint32_t recenterGeneration_{0};
    std::uint32_t fovScalePercent_{
        FEARVR_FOV_SCALE_DEFAULT_PERCENT};
    StereoToggleCallback stereoToggleCallback_{nullptr};
};

Bridge& GetBridge() {
    // Absichtlich prozesslebenslang: CRT-Destruktoren laufen bei DLL_DETACH
    // unter dem Loader-Lock. D3D/IPC-Cleanup darf dort nicht stattfinden;
    // Windows gibt die Prozessressourcen beim Prozessende frei.
    static Bridge* bridge = new Bridge;
    return *bridge;
}

using CreateDeviceFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceExFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
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
using CreateAdditionalSwapChainFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using GetSwapChainFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using SwapChainPresentFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
    const RGNDATA*, DWORD);
using ClearFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR,
    float, DWORD);
using EndSceneFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using DrawPrimitiveFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUpFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUpFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT);
using SetIndicesFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DIndexBuffer9*);
using IndexBufferLockFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD);
using SetRenderTargetFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using StretchRectFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);


using ResetExFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*,
                                D3DPRESENT_PARAMETERS*,
                                D3DDISPLAYMODEEX*);
using PresentExFunction =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*,
                                const RECT*, HWND, const RGNDATA*,
                                DWORD);

void ForceHighQualitySource(D3DPRESENT_PARAMETERS* parameters) noexcept {
    // Keep Retail's own display mode. The game menu relies on that mode's
    // coordinate system; overriding it breaks the flat VR menu projection.
    (void)parameters;
}

struct D3D9VtableRecord {
    void** vtable{nullptr};
    CreateDeviceFunction createDevice{nullptr};
    CreateDeviceExFunction createDeviceEx{nullptr};
};

struct DeviceVtableRecord {
    void** vtable{nullptr};
    ResetFunction reset{nullptr};
    PresentFunction present{nullptr};
    CreateAdditionalSwapChainFunction createAdditionalSwapChain{nullptr};
    GetSwapChainFunction getSwapChain{nullptr};
    ResetExFunction resetEx{nullptr};
    PresentExFunction presentEx{nullptr};
    CreateTextureFunction createTexture{nullptr};
    CreateVolumeTextureFunction createVolumeTexture{nullptr};
    CreateCubeTextureFunction createCubeTexture{nullptr};
    CreateVertexBufferFunction createVertexBuffer{nullptr};
    CreateIndexBufferFunction createIndexBuffer{nullptr};
};

struct SwapChainVtableRecord {
    void** vtable{nullptr};
    SwapChainPresentFunction present{nullptr};
};

SRWLOCK g_hookLock = SRWLOCK_INIT;
SRWLOCK g_appLocalHookLock = SRWLOCK_INIT;
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
EndSceneFunction g_appLocalEndScene = nullptr;
volatile LONG g_managedResourceTranslationLogged = FALSE;
ClearFunction g_appLocalClear = nullptr;
DrawPrimitiveFunction g_appLocalDrawPrimitive = nullptr;
DrawIndexedPrimitiveFunction g_appLocalDrawIndexedPrimitive = nullptr;
DrawPrimitiveUpFunction g_appLocalDrawPrimitiveUp = nullptr;
DrawIndexedPrimitiveUpFunction g_appLocalDrawIndexedPrimitiveUp = nullptr;
SetIndicesFunction g_appLocalSetIndices = nullptr;
IndexBufferLockFunction g_managedIndexBufferLock = nullptr;
SetRenderTargetFunction g_appLocalSetRenderTarget = nullptr;
StretchRectFunction g_appLocalStretchRect = nullptr;
volatile LONG g_setRenderTargetTraceCount = 0;
volatile LONG g_stretchRectTraceCount = 0;
volatile LONG g_clearLogged = FALSE;
volatile LONG g_drawPrimitiveLogged = FALSE;
volatile LONG g_drawIndexedPrimitiveLogged = FALSE;
volatile LONG g_drawPrimitiveUpLogged = FALSE;
volatile LONG g_drawIndexedPrimitiveUpLogged = FALSE;
volatile LONG g_renderActivitySequence = 0;
volatile LONG g_lastClearSequence = 0;
volatile LONG g_lastEndSceneSequence = 0;
volatile LONG g_lastDrawSequence = 0;
volatile LONG g_lastFullscreenDrawSequence = 0;
void* volatile g_lastFullscreenTextureSurface = nullptr;
volatile LONG g_endSceneLogged = FALSE;
volatile LONG g_endSceneAfterDrawLogged = FALSE;
volatile LONG g_endSceneCaptureLogged = FALSE;
volatile LONG g_lastEndSceneCaptureDrawSequence = 0;
volatile LONG g_fullscreenTextureProbePasses = 0;
volatile LONG g_useFullscreenTextureCapture = FALSE;
volatile LONG g_fullscreenTextureCaptureLogged = FALSE;
volatile LONG g_fullscreenTextureDescriptionLogged = FALSE;
volatile LONG g_presentOrderLogged = FALSE;
volatile LONG g_presentCaptureSuppressedLogged = FALSE;
void* volatile g_lastDrawDevice = nullptr;
void* volatile g_managedIndexBuffer = nullptr;
volatile LONG g_managedIndexCreateDetailsLogged = FALSE;
volatile LONG g_managedIndexLockLogged = FALSE;
volatile LONG g_managedIndexSetLogged = FALSE;
volatile LONG g_swapChainHookLogged = FALSE;
thread_local bool g_endSceneCaptureActive = false;
LONG g_managedResourceSuccessLogged[5]{};
struct TrackedRenderTarget {
    IDirect3DDevice9* device{nullptr};
    IDirect3DSurface9* surface{nullptr};
    LONG id{0};
};
SRWLOCK g_renderTargetTraceLock = SRWLOCK_INIT;
std::array<TrackedRenderTarget, 64> g_engineRenderTargets{};
volatile LONG g_engineRenderTargetCount = 0;
volatile LONG g_engineSurfaceProbeCadence = 0;
volatile LONG g_engineSurfaceProbeCursor = 0;
void* volatile g_selectedEngineRenderTarget = nullptr;
volatile LONG g_engineRenderTargetCaptureLogged = FALSE;

void ReleaseTrackedRenderTargets() noexcept {
    std::array<IDirect3DSurface9*, 64> surfaces{};
    AcquireSRWLockExclusive(&g_renderTargetTraceLock);
    InterlockedExchangePointer(
        &g_selectedEngineRenderTarget, nullptr);
    const LONG targetCount = InterlockedExchange(
        &g_engineRenderTargetCount, 0);
    for (LONG index = 0;
         index < targetCount &&
         index < static_cast<LONG>(g_engineRenderTargets.size());
         ++index) {
        TrackedRenderTarget& target =
            g_engineRenderTargets[static_cast<std::size_t>(index)];
        surfaces[static_cast<std::size_t>(index)] = target.surface;
        target = {};
    }
    InterlockedExchange(&g_engineSurfaceProbeCadence, 0);
    InterlockedExchange(&g_engineSurfaceProbeCursor, 0);
    InterlockedExchange(&g_engineRenderTargetCaptureLogged, FALSE);
    ReleaseSRWLockExclusive(&g_renderTargetTraceLock);

    // Release outside the registry lock in case a driver callback re-enters
    // bridge code while the final surface reference is being destroyed.
    for (IDirect3DSurface9* surface : surfaces) {
        if (surface != nullptr) {
            surface->Release();
        }
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
HRESULT STDMETHODCALLTYPE HookCreateAdditionalSwapChain(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters,
    IDirect3DSwapChain9** output);
HRESULT STDMETHODCALLTYPE HookGetSwapChain(
    IDirect3DDevice9* self, UINT index, IDirect3DSwapChain9** output);
HRESULT STDMETHODCALLTYPE HookSwapChainPresent(
    IDirect3DSwapChain9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion, DWORD flags);
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
HRESULT STDMETHODCALLTYPE HookAppLocalReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE HookAppLocalPresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion);
HRESULT STDMETHODCALLTYPE HookAppLocalClear(
    IDirect3DDevice9* self, DWORD count, const D3DRECT* rectangles,
    DWORD flags, D3DCOLOR color, float depth, DWORD stencil);
HRESULT STDMETHODCALLTYPE HookAppLocalEndScene(IDirect3DDevice9* self);
HRESULT STDMETHODCALLTYPE HookAppLocalDrawPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT startVertex, UINT primitiveCount);
HRESULT STDMETHODCALLTYPE HookAppLocalDrawIndexedPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex, UINT minimumVertexIndex, UINT vertexCount,
    UINT startIndex, UINT primitiveCount);
HRESULT STDMETHODCALLTYPE HookAppLocalDrawPrimitiveUp(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount, const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride);
HRESULT STDMETHODCALLTYPE HookAppLocalDrawIndexedPrimitiveUp(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT minimumVertexIndex, UINT vertexCount, UINT primitiveCount,
    const void* indexData, D3DFORMAT indexDataFormat,
    const void* vertexStreamZeroData, UINT vertexStreamZeroStride);
HRESULT STDMETHODCALLTYPE HookAppLocalSetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9* indexBuffer);
HRESULT STDMETHODCALLTYPE HookAppLocalSetRenderTarget(
    IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface);
HRESULT STDMETHODCALLTYPE HookAppLocalStretchRect(
    IDirect3DDevice9* self, IDirect3DSurface9* source,
    const RECT* sourceRect, IDirect3DSurface9* destination,
    const RECT* destinationRect, D3DTEXTUREFILTERTYPE filter);

HRESULT STDMETHODCALLTYPE HookManagedIndexBufferLock(
    IDirect3DIndexBuffer9* self, UINT offset, UINT size, void** data,
    DWORD flags);

HRESULT STDMETHODCALLTYPE HookLateReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters);
HRESULT STDMETHODCALLTYPE HookLatePresent(
    IDirect3DDevice9* self, const RECT* source,
    const RECT* destination, HWND overrideWindow,
    const RGNDATA* dirtyRegion);

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

bool InstallAppLocalImplementationHooks(
    void* resetTarget, void* presentTarget, void* clearTarget,
    void* endSceneTarget, void* drawPrimitiveTarget,
    void* drawIndexedPrimitiveTarget, void* drawPrimitiveUpTarget,
    void* drawIndexedPrimitiveUpTarget, void* setIndicesTarget,
    void* setRenderTargetTarget, void* stretchRectTarget) noexcept {
    const bool requireSetIndices = D3D9ExCompatibilityRequested();
    if (resetTarget == nullptr || presentTarget == nullptr ||
        (requireSetIndices && setIndicesTarget == nullptr)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_appLocalHookLock);
    if (g_appLocalReset != nullptr && g_appLocalPresent != nullptr) {
        ReleaseSRWLockExclusive(&g_appLocalHookLock);
        return true;
    }

    auto fail = [&](const char* event, MH_STATUS status) {
        g_appLocalReset = nullptr;
        g_appLocalPresent = nullptr;
        g_appLocalSetIndices = nullptr;
        ReleaseSRWLockExclusive(&g_appLocalHookLock);
        GetBridge().LogHookStatus(
            "ERROR", event, MH_StatusToString(status));
        return false;
    };

    const MH_STATUS initialize = MH_Initialize();
    if (initialize != MH_OK &&
        initialize != MH_ERROR_ALREADY_INITIALIZED) {
        return fail("app_local_hook_initialize_failed", initialize);
    }

    MH_STATUS status = MH_CreateHook(
        resetTarget,
        reinterpret_cast<void*>(&HookAppLocalReset),
        reinterpret_cast<void**>(&g_appLocalReset));
    if (status != MH_OK) {
        return fail("app_local_reset_hook_failed", status);
    }

    status = MH_CreateHook(
        presentTarget,
        reinterpret_cast<void*>(&HookAppLocalPresent),
        reinterpret_cast<void**>(&g_appLocalPresent));
    if (status != MH_OK) {
        MH_RemoveHook(resetTarget);
        return fail("app_local_present_hook_failed", status);
    }

    if (requireSetIndices) {
        status = MH_CreateHook(
            setIndicesTarget,
            reinterpret_cast<void*>(&HookAppLocalSetIndices),
            reinterpret_cast<void**>(&g_appLocalSetIndices));
        if (status != MH_OK) {
            MH_RemoveHook(presentTarget);
            MH_RemoveHook(resetTarget);
            return fail("app_local_set_indices_hook_failed", status);
        }
    }

    status = MH_QueueEnableHook(resetTarget);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(presentTarget);
    }
    if (status == MH_OK && requireSetIndices) {
        status = MH_QueueEnableHook(setIndicesTarget);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        if (requireSetIndices) {
            MH_RemoveHook(setIndicesTarget);
        }
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resetTarget);
        return fail("app_local_hook_enable_failed", status);
    }

    ReleaseSRWLockExclusive(&g_appLocalHookLock);
    const auto installDiagnosticHook = [](
        void* target, void* detour, void** original,
        const char* failureEvent) {
        MH_STATUS diagnosticStatus =
            MH_CreateHook(target, detour, original);
        if (diagnosticStatus == MH_OK) {
            diagnosticStatus = MH_EnableHook(target);
        }
        if (diagnosticStatus != MH_OK) {
            *original = nullptr;
            GetBridge().LogHookStatus(
                "WARN", failureEvent,
                MH_StatusToString(diagnosticStatus));
        }
    };
    installDiagnosticHook(
        clearTarget, reinterpret_cast<void*>(&HookAppLocalClear),
        reinterpret_cast<void**>(&g_appLocalClear),
        "app_local_clear_probe_failed");
    installDiagnosticHook(
        endSceneTarget, reinterpret_cast<void*>(&HookAppLocalEndScene),
        reinterpret_cast<void**>(&g_appLocalEndScene),
        "app_local_end_scene_probe_failed");
    installDiagnosticHook(
        drawPrimitiveTarget,
        reinterpret_cast<void*>(&HookAppLocalDrawPrimitive),
        reinterpret_cast<void**>(&g_appLocalDrawPrimitive),
        "app_local_draw_primitive_probe_failed");
    installDiagnosticHook(
        drawIndexedPrimitiveTarget,
        reinterpret_cast<void*>(&HookAppLocalDrawIndexedPrimitive),
        reinterpret_cast<void**>(&g_appLocalDrawIndexedPrimitive),
        "app_local_draw_indexed_probe_failed");
    installDiagnosticHook(
        drawPrimitiveUpTarget,
        reinterpret_cast<void*>(&HookAppLocalDrawPrimitiveUp),
        reinterpret_cast<void**>(&g_appLocalDrawPrimitiveUp),
        "app_local_draw_primitive_up_probe_failed");
    installDiagnosticHook(
        drawIndexedPrimitiveUpTarget,
        reinterpret_cast<void*>(&HookAppLocalDrawIndexedPrimitiveUp),
        reinterpret_cast<void**>(&g_appLocalDrawIndexedPrimitiveUp),
        "app_local_draw_indexed_up_probe_failed");
    if (!requireSetIndices) {
        installDiagnosticHook(
            setIndicesTarget,
            reinterpret_cast<void*>(&HookAppLocalSetIndices),
            reinterpret_cast<void**>(&g_appLocalSetIndices),
            "app_local_set_indices_probe_failed");
    } else {
        GetBridge().LogHookStatus(
            "INFO", "app_local_set_indices_required",
            "Managed index-buffer wrappers are unwrapped by a required "
            "SetIndices implementation hook.");
    }
    installDiagnosticHook(
        setRenderTargetTarget,
        reinterpret_cast<void*>(&HookAppLocalSetRenderTarget),
        reinterpret_cast<void**>(&g_appLocalSetRenderTarget),
        "app_local_set_render_target_probe_failed");
    installDiagnosticHook(
        stretchRectTarget,
        reinterpret_cast<void*>(&HookAppLocalStretchRect),
        reinterpret_cast<void**>(&g_appLocalStretchRect),
        "app_local_stretch_rect_probe_failed");

    GetBridge().LogHookStatus(
        "INFO", "app_local_present_detour",
        "System D3D9 Reset and Present implementations are intercepted "
        "inside the app-local proxy; cached engine calls are covered.");
    return true;
}

void PatchSwapChain(IDirect3DSwapChain9* swapChain) noexcept {
    if (swapChain == nullptr) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    bool patched = false;
    AcquireSRWLockExclusive(&g_hookLock);
    for (const SwapChainVtableRecord& record : g_swapChainRecords) {
        if (record.vtable == vtable) {
            ReleaseSRWLockExclusive(&g_hookLock);
            return;
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
}

bool PatchDevice(
    IDirect3DDevice9* device, bool hasEx = false,
    bool translateManagedResources = false) noexcept {
    if (device == nullptr) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(device);
    const bool lateHooksActive =
        InterlockedCompareExchange(
            &g_lateHooksActive, FALSE, FALSE) != FALSE;
    const bool appLocalHooksActive = !lateHooksActive &&
        InstallAppLocalImplementationHooks(
            vtable[16], vtable[17], vtable[43], vtable[42], vtable[81],
            vtable[82], vtable[83], vtable[84], vtable[104],
            vtable[37], vtable[34]);
    const bool implementationHooksActive =
        lateHooksActive || appLocalHooksActive;
    if (translateManagedResources &&
        (!appLocalHooksActive || g_appLocalSetIndices == nullptr)) {
        GetBridge().LogHookStatus(
            "ERROR", "d3d9ex_required_hooks_missing",
            "D3D9Ex compatibility was rejected because the required "
            "app-local SetIndices unwrapping hook is unavailable.");
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
            selected->createAdditionalSwapChain =
                reinterpret_cast<CreateAdditionalSwapChainFunction>(
                    vtable[13]);
            selected->getSwapChain =
                reinterpret_cast<GetSwapChainFunction>(vtable[14]);
            selected->reset = implementationHooksActive
                ? (lateHooksActive ? g_lateReset : g_appLocalReset)
                : reinterpret_cast<ResetFunction>(vtable[16]);
            selected->present = implementationHooksActive
                ? (lateHooksActive ? g_latePresent : g_appLocalPresent)
                : reinterpret_cast<PresentFunction>(vtable[17]);
            ReplaceVtableEntry(
                vtable, 13,
                reinterpret_cast<void*>(&HookCreateAdditionalSwapChain));
            ReplaceVtableEntry(
                vtable, 14, reinterpret_cast<void*>(&HookGetSwapChain));
            if (!implementationHooksActive) {
                ReplaceVtableEntry(
                    vtable, 16,
                    reinterpret_cast<void*>(&HookReset));
                ReplaceVtableEntry(
                    vtable, 17,
                    reinterpret_cast<void*>(&HookPresent));
            }
            break;
        }
    }
    if (selected != nullptr && translateManagedResources &&
        selected->createTexture == nullptr) {
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
        ReplaceVtableEntry(
            vtable, 23, reinterpret_cast<void*>(&HookCreateTexture));
        ReplaceVtableEntry(
            vtable, 24, reinterpret_cast<void*>(&HookCreateVolumeTexture));
        ReplaceVtableEntry(
            vtable, 25, reinterpret_cast<void*>(&HookCreateCubeTexture));
        ReplaceVtableEntry(
            vtable, 26, reinterpret_cast<void*>(&HookCreateVertexBuffer));
        ReplaceVtableEntry(
            vtable, 27, reinterpret_cast<void*>(&HookCreateIndexBuffer));
    }
    if (selected != nullptr && hasEx &&
        selected->presentEx == nullptr) {
        selected->presentEx =
            reinterpret_cast<PresentExFunction>(vtable[121]);
        selected->resetEx =
            reinterpret_cast<ResetExFunction>(vtable[132]);
        ReplaceVtableEntry(
            vtable, 121, reinterpret_cast<void*>(&HookPresentEx));
        ReplaceVtableEntry(
            vtable, 132, reinterpret_cast<void*>(&HookResetEx));
    }
    const GetSwapChainFunction getSwapChain =
        selected == nullptr ? nullptr : selected->getSwapChain;
    ReleaseSRWLockExclusive(&g_hookLock);

    IDirect3DSwapChain9* defaultSwapChain = nullptr;
    if (getSwapChain != nullptr &&
        SUCCEEDED(getSwapChain(device, 0, &defaultSwapChain)) &&
        defaultSwapChain != nullptr) {
        PatchSwapChain(defaultSwapChain);
        defaultSwapChain->Release();
    }
    return selected != nullptr;
}

void PatchD3D9(IDirect3D9* direct3D, bool hasEx) noexcept {
    if (direct3D == nullptr) {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(direct3D);
    AcquireSRWLockExclusive(&g_hookLock);
    for (const D3D9VtableRecord& record : g_d3d9Records) {
        if (record.vtable == vtable) {
            ReleaseSRWLockExclusive(&g_hookLock);
            return;
        }
    }
    for (D3D9VtableRecord& record : g_d3d9Records) {
        if (record.vtable == nullptr) {
            record.vtable = vtable;
            record.createDevice = reinterpret_cast<CreateDeviceFunction>(
                vtable[16]);
            ReplaceVtableEntry(vtable, 16,
                               reinterpret_cast<void*>(&HookCreateDevice));
            if (hasEx) {
                record.createDeviceEx =
                    reinterpret_cast<CreateDeviceExFunction>(vtable[20]);
                ReplaceVtableEntry(
                    vtable, 20,
                    reinterpret_cast<void*>(&HookCreateDeviceEx));
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
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

void LogPresentHookHresult(
    const char* event, HRESULT result, const char* path) noexcept {
    std::ostringstream message;
    message << "path=" << (path == nullptr ? "unknown" : path)
            << " HRESULT=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(result);
    GetBridge().LogHookStatus("ERROR", event, message.str());
}

void CaptureSwapChainBeforePresent(
    IDirect3DSwapChain9* swapChain, HWND destinationWindow,
    const char* path) noexcept {
    if (swapChain == nullptr) {
        return;
    }
    ComPtr<IDirect3DDevice9> device;
    HRESULT result = swapChain->GetDevice(device.GetAddressOf());
    if (FAILED(result) || !device) {
        LogPresentHookHresult(
            "present_swapchain_get_device_failed", result, path);
        return;
    }
    ComPtr<IDirect3DSurface9> backBuffer;
    result = swapChain->GetBackBuffer(
        0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
    if (FAILED(result) || !backBuffer) {
        LogPresentHookHresult(
            "present_swapchain_get_backbuffer_failed", result, path);
        return;
    }
    GetBridge().CapturePresent(
        device.Get(), backBuffer.Get(), swapChain, destinationWindow,
        path);
}

void CaptureDeviceBeforePresent(
    IDirect3DDevice9* device, HWND destinationWindow,
    const char* path) noexcept {
    if (device == nullptr) {
        return;
    }
    ComPtr<IDirect3DSwapChain9> swapChain;
    const HRESULT result =
        device->GetSwapChain(0, swapChain.GetAddressOf());
    if (FAILED(result) || !swapChain) {
        LogPresentHookHresult(
            "present_device_get_swapchain_failed", result, path);
        return;
    }
    CaptureSwapChainBeforePresent(
        swapChain.Get(), destinationWindow, path);
}

void CaptureDeviceSurfaceBeforePresent(
    IDirect3DDevice9* device, IDirect3DSurface9* sourceSurface,
    HWND destinationWindow, const char* path) noexcept {
    if (device == nullptr || sourceSurface == nullptr) {
        return;
    }
    ComPtr<IDirect3DSwapChain9> swapChain;
    const HRESULT result =
        device->GetSwapChain(0, swapChain.GetAddressOf());
    if (FAILED(result) || !swapChain) {
        LogPresentHookHresult(
            "present_device_get_swapchain_failed", result, path);
        return;
    }
    GetBridge().CapturePresent(
        device, sourceSurface, swapChain.Get(), destinationWindow,
        path);
}

bool GetBoundFullscreenTextureSurface(
    IDirect3DDevice9* device,
    IDirect3DSurface9** outputSurface) noexcept {
    if (device == nullptr || outputSurface == nullptr) {
        return false;
    }
    *outputSurface = nullptr;

    ComPtr<IDirect3DBaseTexture9> baseTexture;
    HRESULT result = device->GetTexture(
        0, baseTexture.GetAddressOf());
    if (FAILED(result) || !baseTexture ||
        baseTexture->GetType() != D3DRTYPE_TEXTURE) {
        return false;
    }
    ComPtr<IDirect3DTexture9> texture;
    result = baseTexture.As(&texture);
    if (FAILED(result) || !texture) {
        return false;
    }
    ComPtr<IDirect3DSurface9> textureSurface;
    result = texture->GetSurfaceLevel(
        0, textureSurface.GetAddressOf());
    if (FAILED(result) || !textureSurface) {
        LogPresentHookHresult(
            "fullscreen_texture_get_surface_failed",
            result, "app_local_end_scene_after_draw");
        return false;
    }

    D3DSURFACE_DESC textureDescription{};
    const HRESULT textureDescriptionResult =
        textureSurface->GetDesc(&textureDescription);
    if (FAILED(textureDescriptionResult) ||
        textureDescription.Width == 0 ||
        textureDescription.Height == 0) {
        return false;
    }
    if (InterlockedCompareExchange(
            &g_fullscreenTextureDescriptionLogged,
            TRUE, FALSE) == FALSE) {
        std::ostringstream message;
        message << "size=" << textureDescription.Width << 'x'
                << textureDescription.Height
                << " format="
                << static_cast<unsigned>(textureDescription.Format)
                << " msaa="
                << static_cast<unsigned>(
                       textureDescription.MultiSampleType);
        GetBridge().LogHookStatus(
            "INFO", "final_fullscreen_texture_desc",
            message.str());
    }
    if (textureDescription.Width < 64 ||
        textureDescription.Height < 64) {
        return false;
    }

    *outputSurface = textureSurface.Detach();
    return true;
}

bool GetLastFullscreenTextureSurface(
    IDirect3DSurface9** outputSurface) noexcept {
    if (outputSurface == nullptr) {
        return false;
    }
    *outputSurface = nullptr;
    auto* const surface =
        static_cast<IDirect3DSurface9*>(
            InterlockedCompareExchangePointer(
                &g_lastFullscreenTextureSurface,
                nullptr, nullptr));
    if (surface == nullptr) {
        return false;
    }
    surface->AddRef();
    *outputSurface = surface;
    return true;
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
    if (SUCCEEDED(result) && output != nullptr) {
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
    const HRESULT result =
        record.createAdditionalSwapChain(self, parameters, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) {
        PatchSwapChain(*output);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetSwapChain(
    IDirect3DDevice9* self, UINT index, IDirect3DSwapChain9** output) {
    const DeviceVtableRecord record =
        FindDeviceRecord(*reinterpret_cast<void***>(self));
    if (record.getSwapChain == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = record.getSwapChain(self, index, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) {
        PatchSwapChain(*output);
    }
    return result;
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
    CaptureSwapChainBeforePresent(
        self, overrideWindow, "swapchain_present");
    return record.present(
        self, source, destination, overrideWindow, dirtyRegion, flags);
}

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
std::array<ManagedIndexBufferCompat*, 64>
    g_managedIndexBuffers{};
volatile LONG g_nextManagedIndexDebugId = 0;
volatile LONG g_managedIndexLifecycleSequence = 0;

class ManagedIndexBufferCompat final : public IDirect3DIndexBuffer9 {
public:
    ManagedIndexBufferCompat(
        IDirect3DIndexBuffer9* inner,
        DWORD requestedUsage, D3DFORMAT format,
        UINT length) noexcept
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
            InterlockedCompareExchangePointer(
                &g_managedIndexBuffer, nullptr, this);
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
    volatile LONG lockLogCount_{0};
    volatile LONG unlockLogCount_{0};
    volatile LONG setIndicesCount_{0};
    volatile LONG innerQueryCount_{0};
    volatile LONG addRefCount_{0};
    volatile LONG releaseCount_{0};
};

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
    std::array<ManagedIndexBufferCompat*, 64> buffers{};
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
    std::array<ManagedIndexBufferCompat*, 64> buffers{};
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
    IDirect3DIndexBuffer9* inner = nullptr;
    HRESULT result = record.createIndexBuffer(
        self, length, usage, format, pool, &inner, sharedHandle);
    if (translated && SUCCEEDED(result) && inner != nullptr) {
        auto* compatibility = new (std::nothrow)
            ManagedIndexBufferCompat(
                inner, requestedUsage, format, length);
        if (compatibility == nullptr) {
            inner->Release();
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
        if (SUCCEEDED(result) && output != nullptr &&
            *output != nullptr) {
            InterlockedExchangePointer(
                &g_managedIndexBuffer, *output);
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
    CaptureDeviceBeforePresent(
        self, overrideWindow, "device_present");
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
    CaptureDeviceBeforePresent(
        self, overrideWindow, "device_present_ex");
    return record.presentEx(
        self, source, destination, overrideWindow, dirtyRegion, flags);
}

HRESULT STDMETHODCALLTYPE HookAppLocalReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    if (g_appLocalReset == nullptr) {
        return D3DERR_INVALIDCALL;
    }
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
    const LONG drawSequence = InterlockedCompareExchange(
        &g_lastDrawSequence, 0, 0);
    if (drawSequence != 0 &&
        InterlockedCompareExchange(
            &g_presentOrderLogged, TRUE, FALSE) == FALSE) {
        const LONG clearSequence = InterlockedCompareExchange(
            &g_lastClearSequence, 0, 0);
        const LONG endSceneSequence = InterlockedCompareExchange(
            &g_lastEndSceneSequence, 0, 0);
        void* const drawDevice = InterlockedCompareExchangePointer(
            &g_lastDrawDevice, nullptr, nullptr);
        std::ostringstream message;
        message << "draw_sequence=" << drawSequence
                << " clear_sequence=" << clearSequence
                << " draw_after_clear="
                << (drawSequence > clearSequence ? 1 : 0)
                << " end_scene_sequence=" << endSceneSequence
                << " end_scene_after_draw="
                << (endSceneSequence > drawSequence ? 1 : 0)
                << " end_scene_before_clear="
                << (endSceneSequence > drawSequence &&
                    endSceneSequence < clearSequence ? 1 : 0)
                << " same_device="
                << (drawDevice == self ? 1 : 0);
        GetBridge().LogHookStatus(
            "INFO", "d3d9_present_order", message.str());
    }
    const LONG endSceneCaptureDrawSequence =
        InterlockedCompareExchange(
            &g_lastEndSceneCaptureDrawSequence, 0, 0);
    const LONG clearSequence = InterlockedCompareExchange(
        &g_lastClearSequence, 0, 0);
    const bool capturedBeforeDestructiveClear =
        drawSequence != 0 &&
        endSceneCaptureDrawSequence == drawSequence &&
        clearSequence > drawSequence;
    if (capturedBeforeDestructiveClear) {
        if (InterlockedCompareExchange(
                &g_presentCaptureSuppressedLogged,
                TRUE, FALSE) == FALSE) {
            GetBridge().LogHookStatus(
                "INFO", "d3d9_present_capture_suppressed",
                "The completed backbuffer was captured after EndScene "
                "before Jupiter EX cleared it; the later black Present "
                "is ignored.");
        }
    } else {
        CaptureDeviceBeforePresent(
            self, overrideWindow, "app_local_device_present");
    }
    return g_appLocalPresent(
        self, source, destination, overrideWindow, dirtyRegion);
}

void LogRenderActivity(
    volatile LONG* logged, const char* event, HRESULT result,
    const std::string& details) noexcept {
    if (InterlockedCompareExchange(logged, TRUE, FALSE) != FALSE) {
        return;
    }
    std::ostringstream message;
    message << details << " HRESULT=0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(result);
    GetBridge().LogHookStatus(
        SUCCEEDED(result) ? "INFO" : "ERROR", event, message.str());
}

void AppendDrawState(
    IDirect3DDevice9* device, std::ostringstream& details) noexcept {
    ComPtr<IDirect3DSurface9> target;
    ComPtr<IDirect3DSurface9> backBuffer;
    const HRESULT targetResult =
        device->GetRenderTarget(0, target.GetAddressOf());
    const HRESULT backBufferResult = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
    D3DSURFACE_DESC targetDescription{};
    const HRESULT descriptionResult = target
        ? target->GetDesc(&targetDescription)
        : D3DERR_INVALIDCALL;

    D3DVIEWPORT9 viewport{};
    const HRESULT viewportResult = device->GetViewport(&viewport);
    DWORD colorWrite = 0;
    const HRESULT colorWriteResult =
        device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);

    ComPtr<IDirect3DBaseTexture9> texture;
    const HRESULT textureResult =
        device->GetTexture(0, texture.GetAddressOf());
    ComPtr<IDirect3DVertexShader9> vertexShader;
    const HRESULT vertexShaderResult =
        device->GetVertexShader(vertexShader.GetAddressOf());
    ComPtr<IDirect3DPixelShader9> pixelShader;
    const HRESULT pixelShaderResult =
        device->GetPixelShader(pixelShader.GetAddressOf());

    details << " target_hr=0x" << std::hex
            << static_cast<std::uint32_t>(targetResult)
            << " backbuffer_hr=0x"
            << static_cast<std::uint32_t>(backBufferResult)
            << " target_is_backbuffer="
            << (target && backBuffer &&
                target.Get() == backBuffer.Get() ? 1 : 0)
            << " target_desc_hr=0x"
            << static_cast<std::uint32_t>(descriptionResult)
            << std::dec
            << " target=" << targetDescription.Width << 'x'
            << targetDescription.Height
            << " format="
            << static_cast<unsigned>(targetDescription.Format)
            << " msaa="
            << static_cast<unsigned>(
                   targetDescription.MultiSampleType)
            << " viewport_hr=0x" << std::hex
            << static_cast<std::uint32_t>(viewportResult)
            << std::dec
            << " viewport=" << viewport.X << ',' << viewport.Y
            << ',' << viewport.Width << 'x' << viewport.Height
            << " colorwrite_hr=0x" << std::hex
            << static_cast<std::uint32_t>(colorWriteResult)
            << " colorwrite=0x" << colorWrite
            << " texture_hr=0x"
            << static_cast<std::uint32_t>(textureResult)
            << " texture=" << (texture ? 1 : 0)
            << " vs_hr=0x"
            << static_cast<std::uint32_t>(vertexShaderResult)
            << " vs=" << (vertexShader ? 1 : 0)
            << " ps_hr=0x"
            << static_cast<std::uint32_t>(pixelShaderResult)
            << " ps=" << (pixelShader ? 1 : 0)
            << std::dec;
}

bool GetSelectedEngineRenderTarget(
    IDirect3DDevice9* device,
    IDirect3DSurface9** outputSurface) noexcept {
    if (device == nullptr || outputSurface == nullptr) {
        return false;
    }
    *outputSurface = nullptr;
    ComPtr<IDirect3DSurface9> selected;
    AcquireSRWLockShared(&g_renderTargetTraceLock);
    auto* const selectedPointer =
        static_cast<IDirect3DSurface9*>(
            InterlockedCompareExchangePointer(
                &g_selectedEngineRenderTarget,
                nullptr, nullptr));
    if (selectedPointer != nullptr) {
        selectedPointer->AddRef();
        selected.Attach(selectedPointer);
    }
    ReleaseSRWLockShared(&g_renderTargetTraceLock);
    if (!selected) {
        return false;
    }
    ComPtr<IDirect3DDevice9> surfaceDevice;
    if (FAILED(selected->GetDevice(
            surfaceDevice.GetAddressOf())) ||
        surfaceDevice.Get() != device) {
        return false;
    }
    *outputSurface = selected.Detach();
    return true;
}

bool ProbeNextEngineRenderTarget(
    IDirect3DDevice9* device,
    IDirect3DSurface9** outputSurface) noexcept {
    if (device == nullptr || outputSurface == nullptr) {
        return false;
    }
    *outputSurface = nullptr;
    const LONG cadence =
        InterlockedIncrement(&g_engineSurfaceProbeCadence);
    if (cadence != 1 && cadence % 10 != 0) {
        return false;
    }

    const LONG targetCount = InterlockedCompareExchange(
        &g_engineRenderTargetCount, 0, 0);
    if (targetCount <= 0) {
        return false;
    }
    const LONG cursor =
        InterlockedIncrement(&g_engineSurfaceProbeCursor) - 1;
    if (cursor >= targetCount * 4) {
        return false;
    }
    const LONG targetIndex = cursor % targetCount;

    ComPtr<IDirect3DSurface9> candidate;
    LONG candidateId = 0;
    AcquireSRWLockShared(&g_renderTargetTraceLock);
    const TrackedRenderTarget& tracked =
        g_engineRenderTargets[
            static_cast<std::size_t>(targetIndex)];
    if (tracked.device == device && tracked.surface != nullptr) {
        tracked.surface->AddRef();
        candidate.Attach(tracked.surface);
        candidateId = tracked.id;
    }
    ReleaseSRWLockShared(&g_renderTargetTraceLock);
    if (!candidate) {
        return false;
    }

    ComPtr<IDirect3DSurface9> backBuffer;
    D3DSURFACE_DESC candidateDescription{};
    D3DSURFACE_DESC backBufferDescription{};
    if (FAILED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO,
            backBuffer.GetAddressOf())) ||
        !backBuffer ||
        FAILED(candidate->GetDesc(&candidateDescription)) ||
        FAILED(backBuffer->GetDesc(&backBufferDescription)) ||
        candidateDescription.Width != backBufferDescription.Width ||
        candidateDescription.Height != backBufferDescription.Height ||
        candidateDescription.Format != backBufferDescription.Format) {
        return false;
    }

    const std::string probeName =
        "tracked_render_target_" + std::to_string(candidateId);
    if (!GetBridge().ProbeDiagnosticSurface(
            device, candidate.Get(), probeName.c_str())) {
        return false;
    }

    bool stillTracked = false;
    AcquireSRWLockShared(&g_renderTargetTraceLock);
    for (const TrackedRenderTarget& registryEntry :
         g_engineRenderTargets) {
        if (registryEntry.device == device &&
            registryEntry.surface == candidate.Get()) {
            InterlockedExchangePointer(
                &g_selectedEngineRenderTarget, candidate.Get());
            stillTracked = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_renderTargetTraceLock);
    if (!stillTracked) {
        return false;
    }
    if (InterlockedCompareExchange(
            &g_engineRenderTargetCaptureLogged,
            TRUE, FALSE) == FALSE) {
        GetBridge().LogHookStatus(
            "WARN", "tracked_render_target_capture_selected",
            "A tracked full-size engine render target contains RGB "
            "pixels while the presentation backbuffer is black; "
            "capture now follows that persistent surface.");
    }
    *outputSurface = candidate.Detach();
    return true;
}

HRESULT STDMETHODCALLTYPE HookAppLocalClear(
    IDirect3DDevice9* self, DWORD count, const D3DRECT* rectangles,
    DWORD flags, D3DCOLOR color, float depth, DWORD stencil) {
    if (g_appLocalClear == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalClear(
        self, count, rectangles, flags, color, depth, stencil);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastClearSequence, sequence);
    std::ostringstream details;
    details << "flags=0x" << std::hex << std::uppercase << flags
            << " color=0x" << color << std::dec
            << " rectangles=" << count;
    LogRenderActivity(
        &g_clearLogged, "d3d9_clear_activity", result, details.str());
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalEndScene(IDirect3DDevice9* self) {
    if (g_appLocalEndScene == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalEndScene(self);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastEndSceneSequence, sequence);
    const LONG drawSequence = InterlockedCompareExchange(
        &g_lastDrawSequence, 0, 0);
    const LONG clearSequence = InterlockedCompareExchange(
        &g_lastClearSequence, 0, 0);
    void* const drawDevice = InterlockedCompareExchangePointer(
        &g_lastDrawDevice, nullptr, nullptr);
    std::ostringstream details;
    details << "sequence=" << sequence
            << " draw_sequence=" << drawSequence
            << " clear_sequence=" << clearSequence
            << " after_draw=" << (sequence > drawSequence ? 1 : 0)
            << " draw_after_clear="
            << (drawSequence > clearSequence ? 1 : 0)
            << " same_device=" << (drawDevice == self ? 1 : 0);
    LogRenderActivity(
        &g_endSceneLogged, "d3d9_end_scene_activity",
        result, details.str());
    if (drawSequence != 0) {
        LogRenderActivity(
            &g_endSceneAfterDrawLogged,
            "d3d9_end_scene_after_draw",
            result, details.str());
    }
    const LONG capturedDrawSequence = InterlockedCompareExchange(
        &g_lastEndSceneCaptureDrawSequence, 0, 0);
    const bool completedDrawPass =
        SUCCEEDED(result) &&
        drawSequence > clearSequence &&
        drawDevice == self &&
        capturedDrawSequence != drawSequence;
    if (completedDrawPass &&
        !GetBridge().ShouldDeferEndSceneCapture() &&
        !g_endSceneCaptureActive) {
        g_endSceneCaptureActive = true;
        ComPtr<IDirect3DSurface9> trackedRenderTarget;
        bool useTrackedRenderTarget =
            GetSelectedEngineRenderTarget(
                self, trackedRenderTarget.GetAddressOf());
        if (!useTrackedRenderTarget) {
            useTrackedRenderTarget =
                ProbeNextEngineRenderTarget(
                    self, trackedRenderTarget.GetAddressOf());
        }
        ComPtr<IDirect3DSurface9> fullscreenTextureSurface;
        const LONG fullscreenDrawSequence =
            InterlockedCompareExchange(
                &g_lastFullscreenDrawSequence, 0, 0);
        const bool hasFullscreenTexture =
            D3D9ExCompatibilityRequested() &&
            fullscreenDrawSequence > clearSequence &&
            fullscreenDrawSequence <= drawSequence &&
            GetLastFullscreenTextureSurface(
                fullscreenTextureSurface.GetAddressOf());
        bool useFullscreenTexture =
            hasFullscreenTexture &&
            InterlockedCompareExchange(
                &g_useFullscreenTextureCapture,
                FALSE, FALSE) != FALSE;
        if (hasFullscreenTexture && !useFullscreenTexture) {
            const LONG probePass = InterlockedIncrement(
                &g_fullscreenTextureProbePasses);
            if (probePass == 1 || probePass % 60 == 0) {
                useFullscreenTexture =
                    GetBridge().ProbeDiagnosticSurface(
                        self, fullscreenTextureSurface.Get(),
                        "final_fullscreen_texture");
                if (useFullscreenTexture) {
                    InterlockedExchange(
                        &g_useFullscreenTextureCapture, TRUE);
                    if (InterlockedCompareExchange(
                            &g_fullscreenTextureCaptureLogged,
                            TRUE, FALSE) == FALSE) {
                        GetBridge().LogHookStatus(
                            "WARN",
                            "fullscreen_texture_capture_selected",
                            "The final fullscreen draw sampled a "
                            "nonblack full-size texture while its "
                            "backbuffer output stayed black; capture "
                            "now follows that composite texture.");
                    }
                }
            }
        }
        if (useTrackedRenderTarget) {
            CaptureDeviceSurfaceBeforePresent(
                self, trackedRenderTarget.Get(), nullptr,
                "app_local_end_scene_tracked_render_target");
        } else if (useFullscreenTexture) {
            CaptureDeviceSurfaceBeforePresent(
                self, fullscreenTextureSurface.Get(), nullptr,
                "app_local_end_scene_fullscreen_texture");
        } else {
            CaptureDeviceBeforePresent(
                self, nullptr,
                "app_local_end_scene_after_draw");
        }
        g_endSceneCaptureActive = false;
        InterlockedExchange(
            &g_lastEndSceneCaptureDrawSequence, drawSequence);
        LogRenderActivity(
            &g_endSceneCaptureLogged,
            "d3d9_end_scene_capture",
            S_OK,
            "Captured the completed backbuffer after EndScene so a later "
            "Jupiter EX color clear cannot erase the mono frame.");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalDrawPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT startVertex, UINT primitiveCount) {
    if (g_appLocalDrawPrimitive == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalDrawPrimitive(
        self, primitiveType, startVertex, primitiveCount);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastDrawSequence, sequence);
    InterlockedExchangePointer(
        &g_lastDrawDevice, self);
    if (SUCCEEDED(result) &&
        primitiveType == D3DPT_TRIANGLESTRIP &&
        primitiveCount == 2) {
        ComPtr<IDirect3DSurface9> target;
        D3DSURFACE_DESC description{};
        D3DVIEWPORT9 viewport{};
        ComPtr<IDirect3DBaseTexture9> texture;
        if (SUCCEEDED(self->GetRenderTarget(
                0, target.GetAddressOf())) &&
            target &&
            SUCCEEDED(target->GetDesc(&description)) &&
            SUCCEEDED(self->GetViewport(&viewport)) &&
            viewport.X == 0 && viewport.Y == 0 &&
            viewport.Width == description.Width &&
            viewport.Height == description.Height &&
            SUCCEEDED(self->GetTexture(
                0, texture.GetAddressOf())) &&
            texture) {
            InterlockedExchange(
                &g_lastFullscreenDrawSequence, sequence);
            ComPtr<IDirect3DSurface9> fullscreenTextureSurface;
            if (GetBoundFullscreenTextureSurface(
                    self,
                    fullscreenTextureSurface.GetAddressOf())) {
                auto* const previous =
                    static_cast<IDirect3DSurface9*>(
                        InterlockedExchangePointer(
                            &g_lastFullscreenTextureSurface,
                            fullscreenTextureSurface.Detach()));
                if (previous != nullptr) {
                    previous->Release();
                }
            }
        }
    }
    std::ostringstream details;
    details << "type=" << static_cast<unsigned>(primitiveType)
            << " start_vertex=" << startVertex
            << " primitives=" << primitiveCount;
    if (InterlockedCompareExchange(
            &g_drawPrimitiveLogged, FALSE, FALSE) == FALSE) {
        AppendDrawState(self, details);
    }
    LogRenderActivity(
        &g_drawPrimitiveLogged, "d3d9_draw_primitive_activity",
        result, details.str());
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalDrawIndexedPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex, UINT minimumVertexIndex, UINT vertexCount,
    UINT startIndex, UINT primitiveCount) {
    if (g_appLocalDrawIndexedPrimitive == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalDrawIndexedPrimitive(
        self, primitiveType, baseVertexIndex, minimumVertexIndex,
        vertexCount, startIndex, primitiveCount);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastDrawSequence, sequence);
    InterlockedExchangePointer(
        &g_lastDrawDevice, self);
    std::ostringstream details;
    details << "type=" << static_cast<unsigned>(primitiveType)
            << " base_vertex=" << baseVertexIndex
            << " min_vertex=" << minimumVertexIndex
            << " vertices=" << vertexCount
            << " start_index=" << startIndex
            << " primitives=" << primitiveCount;
    if (InterlockedCompareExchange(
            &g_drawIndexedPrimitiveLogged, FALSE, FALSE) == FALSE) {
        AppendDrawState(self, details);
    }
    LogRenderActivity(
        &g_drawIndexedPrimitiveLogged,
        "d3d9_draw_indexed_activity", result, details.str());
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalDrawPrimitiveUp(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount, const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride) {
    if (g_appLocalDrawPrimitiveUp == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalDrawPrimitiveUp(
        self, primitiveType, primitiveCount,
        vertexStreamZeroData, vertexStreamZeroStride);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastDrawSequence, sequence);
    InterlockedExchangePointer(&g_lastDrawDevice, self);
    std::ostringstream details;
    details << "type=" << static_cast<unsigned>(primitiveType)
            << " primitives=" << primitiveCount
            << " stride=" << vertexStreamZeroStride
            << " vertex_data="
            << (vertexStreamZeroData != nullptr ? 1 : 0);
    if (InterlockedCompareExchange(
            &g_drawPrimitiveUpLogged, FALSE, FALSE) == FALSE) {
        AppendDrawState(self, details);
    }
    LogRenderActivity(
        &g_drawPrimitiveUpLogged,
        "d3d9_draw_primitive_up_activity",
        result, details.str());
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalDrawIndexedPrimitiveUp(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType,
    UINT minimumVertexIndex, UINT vertexCount, UINT primitiveCount,
    const void* indexData, D3DFORMAT indexDataFormat,
    const void* vertexStreamZeroData, UINT vertexStreamZeroStride) {
    if (g_appLocalDrawIndexedPrimitiveUp == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalDrawIndexedPrimitiveUp(
        self, primitiveType, minimumVertexIndex, vertexCount,
        primitiveCount, indexData, indexDataFormat,
        vertexStreamZeroData, vertexStreamZeroStride);
    const LONG sequence =
        InterlockedIncrement(&g_renderActivitySequence);
    InterlockedExchange(&g_lastDrawSequence, sequence);
    InterlockedExchangePointer(&g_lastDrawDevice, self);
    std::ostringstream details;
    details << "type=" << static_cast<unsigned>(primitiveType)
            << " min_vertex=" << minimumVertexIndex
            << " vertices=" << vertexCount
            << " primitives=" << primitiveCount
            << " index_format="
            << static_cast<unsigned>(indexDataFormat)
            << " stride=" << vertexStreamZeroStride
            << " index_data=" << (indexData != nullptr ? 1 : 0)
            << " vertex_data="
            << (vertexStreamZeroData != nullptr ? 1 : 0);
    if (InterlockedCompareExchange(
            &g_drawIndexedPrimitiveUpLogged, FALSE, FALSE) == FALSE) {
        AppendDrawState(self, details);
    }
    LogRenderActivity(
        &g_drawIndexedPrimitiveUpLogged,
        "d3d9_draw_indexed_up_activity",
        result, details.str());
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalSetIndices(
    IDirect3DDevice9* self, IDirect3DIndexBuffer9* indexBuffer) {
    if (g_appLocalSetIndices == nullptr) {
        return D3DERR_INVALIDCALL;
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
    if (indexBuffer == InterlockedCompareExchangePointer(
            &g_managedIndexBuffer, nullptr, nullptr)) {
        D3DINDEXBUFFER_DESC description{};
        const HRESULT descriptionResult =
            indexBuffer->GetDesc(&description);
        std::ostringstream details;
        details << "desc_hr=0x" << std::hex
                << static_cast<std::uint32_t>(descriptionResult)
                << " usage=0x" << description.Usage << std::dec
                << " pool=" << static_cast<unsigned>(description.Pool)
                << " format="
                << static_cast<unsigned>(description.Format)
                << " size=" << description.Size;
        LogRenderActivity(
            &g_managedIndexSetLogged,
            "d3d9ex_managed_index_set",
            result, details.str());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookManagedIndexBufferLock(
    IDirect3DIndexBuffer9* self, UINT offset, UINT size, void** data,
    DWORD flags) {
    if (g_managedIndexBufferLock == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result =
        g_managedIndexBufferLock(self, offset, size, data, flags);
    if (self == InterlockedCompareExchangePointer(
            &g_managedIndexBuffer, nullptr, nullptr)) {
        std::ostringstream details;
        details << "offset=" << offset << " size=" << size
                << " flags=0x" << std::hex << flags << std::dec
                << " data="
                << (data != nullptr && *data != nullptr ? 1 : 0);
        LogRenderActivity(
            &g_managedIndexLockLogged,
            "d3d9ex_managed_index_lock",
            result, details.str());
    }
    return result;
}
void AppendSurfaceDescription(
    IDirect3DSurface9* surface, std::ostringstream& message) {
    D3DSURFACE_DESC description{};
    const HRESULT result = surface != nullptr
        ? surface->GetDesc(&description)
        : D3DERR_INVALIDCALL;
    message << " ptr=0x" << std::hex
            << reinterpret_cast<std::uintptr_t>(surface)
            << " desc_hr=0x" << static_cast<std::uint32_t>(result)
            << std::dec << " size=" << description.Width << 'x'
            << description.Height << " format="
            << static_cast<unsigned>(description.Format)
            << " msaa="
            << static_cast<unsigned>(description.MultiSampleType);
}

HRESULT STDMETHODCALLTYPE HookAppLocalSetRenderTarget(
    IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface) {
    if (g_appLocalSetRenderTarget == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result =
        g_appLocalSetRenderTarget(self, index, surface);
    const LONG trace =
        InterlockedIncrement(&g_setRenderTargetTraceCount);
    if (!g_endSceneCaptureActive) {
        ComPtr<IDirect3DSurface9> backBuffer;
        self->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO,
            backBuffer.GetAddressOf());
        if (D3D9ExCompatibilityRequested() &&
            SUCCEEDED(result) && index == 0 && surface != nullptr &&
            surface != backBuffer.Get()) {
            AcquireSRWLockExclusive(&g_renderTargetTraceLock);
            bool known = false;
            const LONG targetCount = InterlockedCompareExchange(
                &g_engineRenderTargetCount, 0, 0);
            for (LONG targetIndex = 0;
                 targetIndex < targetCount; ++targetIndex) {
                const TrackedRenderTarget& target =
                    g_engineRenderTargets[
                        static_cast<std::size_t>(targetIndex)];
                if (target.device == self &&
                    target.surface == surface) {
                    known = true;
                    break;
                }
            }
            if (!known && targetCount <
                    static_cast<LONG>(
                        g_engineRenderTargets.size())) {
                surface->AddRef();
                TrackedRenderTarget& target =
                    g_engineRenderTargets[
                        static_cast<std::size_t>(targetCount)];
                target.device = self;
                target.surface = surface;
                target.id = targetCount + 1;
                InterlockedExchange(
                    &g_engineRenderTargetCount,
                    targetCount + 1);
            }
            ReleaseSRWLockExclusive(&g_renderTargetTraceLock);
        }
        if (trace <= 32) {
            std::ostringstream message;
            message << "trace=" << trace << " slot=" << index
                    << " is_backbuffer="
                    << (surface != nullptr &&
                        surface == backBuffer.Get() ? 1 : 0);
            AppendSurfaceDescription(surface, message);
            message << " HRESULT=0x" << std::hex << std::uppercase
                    << static_cast<std::uint32_t>(result);
            GetBridge().LogHookStatus(
                SUCCEEDED(result) ? "INFO" : "ERROR",
                "d3d9_set_render_target", message.str());
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookAppLocalStretchRect(
    IDirect3DDevice9* self, IDirect3DSurface9* source,
    const RECT* sourceRect, IDirect3DSurface9* destination,
    const RECT* destinationRect, D3DTEXTUREFILTERTYPE filter) {
    if (g_appLocalStretchRect == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_appLocalStretchRect(
        self, source, sourceRect, destination, destinationRect, filter);
    const LONG trace =
        InterlockedIncrement(&g_stretchRectTraceCount);
    if (trace <= 32 && !g_endSceneCaptureActive) {
        std::ostringstream message;
        message << "trace=" << trace << " source";
        AppendSurfaceDescription(source, message);
        message << " destination";
        AppendSurfaceDescription(destination, message);
        message << " filter=" << static_cast<unsigned>(filter)
                << " HRESULT=0x" << std::hex << std::uppercase
                << static_cast<std::uint32_t>(result);
        GetBridge().LogHookStatus(
            SUCCEEDED(result) ? "INFO" : "ERROR",
            "d3d9_stretch_rect", message.str());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookLateReset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    if (g_lateReset == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (D3D9ExCompatibilityRequested() && parameters != nullptr) {
        D3DDEVICE_CREATION_PARAMETERS creation{};
        HWND focusWindow = nullptr;
        if (SUCCEEDED(self->GetCreationParameters(&creation))) {
            focusWindow = creation.hFocusWindow;
        }
        TranslateD3D9ExPresentation(focusWindow, parameters);
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
    CaptureDeviceBeforePresent(
        self, overrideWindow, "late_device_present");
    return g_latePresent(self, source, destination, overrideWindow,
                         dirtyRegion);
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
    if (resetTarget == reinterpret_cast<void*>(&HookReset) &&
        presentTarget == reinterpret_cast<void*>(&HookPresent)) {
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

    status = MH_QueueEnableHook(resetTarget);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(presentTarget);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resetTarget);
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
        "System D3D9 Reset and Present hooks are active.");
    device.Reset();
    direct3D.Reset();
    DestroyWindow(window);
    return TRUE;
}

} // namespace

void OnDirect3D9Created(IDirect3D9* direct3D) noexcept {
    PatchD3D9(direct3D, false);
}

void OnDirect3D9ExCreated(IDirect3D9Ex* direct3D) noexcept {
    PatchD3D9(direct3D, true);
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
