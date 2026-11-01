#pragma once
#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "nvapi.h"

namespace vrpacing {

// [A4] Compatibilidad hacia atras para llamadas heredadas de Oculus
enum class VrsTextureMode {
    SingleEye,
    CombinedSideBySide,
    TextureArray
};

// [A4] Unified centered Fixed Foveated Rendering (Hardware VRS via NVIDIA NVAPI)
class VrsFoveatedD3D11 {
public:
    // [F4] Heap-allocated singleton (never destroyed during static teardown)
    static VrsFoveatedD3D11& Instance();

    // [A4] & [F1] Simplified target update: obeys `enabled` strictly as passed by caller.
    // [A8] debugMode: 0=normal, 1=passive, 2=transparent (all 1x1), 3=extreme (outer rings 4x4).
    void UpdateTarget(ID3D11Device* dev,
                      ID3D11DeviceContext* ctx,
                      bool enabled,
                      uint32_t eyeWidth,
                      uint32_t eyeHeight,
                      int debugMode = 0);

    // Sobrecarga de compatibilidad para llamadas heredadas de 10 parametros
    void UpdateTarget(ID3D11Device* dev,
                      ID3D11DeviceContext* ctx,
                      bool enabled,
                      uint32_t eyeWidth,
                      uint32_t eyeHeight,
                      VrsTextureMode /*mode*/,
                      float /*p0x*/ = 0.5f, float /*p0y*/ = 0.5f,
                      float /*p1x*/ = 0.5f, float /*p1y*/ = 0.5f) {
        UpdateTarget(dev, ctx, enabled, eyeWidth, eyeHeight, 0);
    }

    // [A8] Periodic stats logging every 900 frames.
    void EndFrame();

    // [A9] Releases D3D11 textures and intentionally detaches NVAPI SRRVs to avoid teardown crash.
    void Shutdown();

    // [A2] & [F2] Public thread-safe entry point to disable VRS on the registered context.
    void DisableVRS(ID3D11DeviceContext* ctx, bool force = false);

    // [A6] Thread-local suppression scope counter for VRPacing's own rendering/upscaling passes.
    static void EnterSuppressScope() noexcept;
    static void LeaveSuppressScope() noexcept;
    static bool IsSuppressed() noexcept;

    // Invoked by ID3D11DeviceContext vtable hooks (vtbl[33] and vtbl[34]).
    void OnPostOMSetRenderTargets(ID3D11DeviceContext* ctx,
                                  UINT numViews,
                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                  ID3D11DepthStencilView* pDepthStencilView);

private:
    enum class VrsLayout : uint8_t {
        SingleEye = 0,
        Combined = 1,
        Array = 2,
        Count = 3
    };

    struct PatternEntry {
        int tilesW = 0;
        int tilesH = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> view;
    };

    static constexpr size_t kMaxPatternsPerLayout = 4;

    VrsFoveatedD3D11() = default;
    ~VrsFoveatedD3D11() = default; // [F4] Must not call Shutdown() or NVAPI during static teardown

    VrsFoveatedD3D11(const VrsFoveatedD3D11&) = delete;
    VrsFoveatedD3D11& operator=(const VrsFoveatedD3D11&) = delete;

    bool EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void InstallContextHooksLocked(ID3D11DeviceContext* ctx);

    // [F3] Bounded pattern cache lookup/creation (max 4 entries per layout, never recreated)
    ID3D11NvShadingRateResourceView* GetOrCreatePatternLocked(VrsLayout layout, int rtWidth, int rtHeight);
    void ClearPatternCacheLocked();

    void ApplyVRSViewLocked(ID3D11DeviceContext* ctx,
                            ID3D11NvShadingRateResourceView* view,
                            const char* label);
    void EnableVRS(ID3D11DeviceContext* ctx);
    void DisableVRSLocked(ID3D11DeviceContext* ctx, bool force = false);

    void LogUniqueRenderTargetLocked(uint32_t w,
                                     uint32_t h,
                                     uint32_t arraySize,
                                     DXGI_FORMAT format,
                                     uint32_t samples,
                                     D3D11_RTV_DIMENSION rtvDim,
                                     const char* resultLabel);

    void LogNvapiFailureRateLimited(const char* opName, NvAPI_Status status);

    std::mutex mutex_;
    std::atomic<bool> fastEnabled_{ false };
    std::atomic<bool> fastBound_{ false };

    bool nvapiChecked_ = false;
    bool nvapiSupported_ = false;
    bool hooksInstalled_ = false;
    bool hookInstallAttempted_ = false; // [A1] Do not retry failed hook installation every frame

    bool enabled_ = false;
    int debugMode_ = 0; // [A8] 0=normal, 1=passive, 2=transparent, 3=extreme
    bool vrsCurrentlyBound_ = false;
    uint32_t targetEyeWidth_ = 0;
    uint32_t targetEyeHeight_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // [F3] Pattern cache keyed by (layout, tilesW, tilesH), max 4 entries per layout
    std::vector<PatternEntry> patternCache_[static_cast<size_t>(VrsLayout::Count)];
    bool cacheFullLogged_ = false;

    // [A8] Bounded unique RT diagnostic set (max 48 entries) and atomic telemetry counters
    std::unordered_set<uint64_t> loggedUniqueRts_;
    std::atomic<uint64_t> framesCount_{ 0 };
    std::atomic<uint64_t> candidateBinds_{ 0 };
    std::atomic<uint64_t> enableCount_{ 0 };
    std::atomic<uint64_t> disableCount_{ 0 };
    std::atomic<uint64_t> nvFailCount_{ 0 };
    uint32_t nvFailLogCount_ = 0;
};

// [A6] RAII guard to suppress VRS interception during VRPacing's own rendering/upscaling passes
class VrsSuppressScope {
public:
    VrsSuppressScope() noexcept { VrsFoveatedD3D11::EnterSuppressScope(); }
    ~VrsSuppressScope() noexcept { VrsFoveatedD3D11::LeaveSuppressScope(); }

    VrsSuppressScope(const VrsSuppressScope&) = delete;
    VrsSuppressScope& operator=(const VrsSuppressScope&) = delete;
};

} // namespace vrpacing

#endif // _WIN32