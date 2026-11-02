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

enum class VrsTextureMode {
    SingleEye,
    CombinedSideBySide,
    TextureArray
};

// [B4] Global process detach flag set by DllMain
extern std::atomic<bool> g_vrsDetaching;

class VrsFoveatedD3D11 {
public:
    static VrsFoveatedD3D11& Instance();

    void UpdateTarget(ID3D11Device* dev,
                      ID3D11DeviceContext* ctx,
                      bool enabled,
                      uint32_t eyeWidth,
                      uint32_t eyeHeight,
                      int debugMode = 0);

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

    // [B1] Swapchain image exclusion registration
    void RegisterExcludedTexture(ID3D11Texture2D* tex);
    void UnregisterExcludedTexture(ID3D11Texture2D* tex);
    void ClearExcludedTextures();

    void EndFrame();
    void Shutdown(const char* reason = "Explicit");
    void DisableVRS(ID3D11DeviceContext* ctx, bool force = false);

    static void EnterSuppressScope() noexcept;
    static void LeaveSuppressScope() noexcept;
    static bool IsSuppressed() noexcept;

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
    static constexpr size_t kMaxUniqueRtLogs = 64; // [B2]

    VrsFoveatedD3D11() = default;
    ~VrsFoveatedD3D11() = default;

    VrsFoveatedD3D11(const VrsFoveatedD3D11&) = delete;
    VrsFoveatedD3D11& operator=(const VrsFoveatedD3D11&) = delete;

    bool EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void InstallContextHooksLocked(ID3D11DeviceContext* ctx);

    ID3D11NvShadingRateResourceView* GetOrCreatePatternLocked(VrsLayout layout, int rtWidth, int rtHeight);
    void ClearPatternCacheLocked();

    void ApplyVRSViewLocked(ID3D11DeviceContext* ctx,
                            ID3D11NvShadingRateResourceView* view,
                            const char* label);
    void EnableVRS(ID3D11DeviceContext* ctx);
    void DisableVRSLocked(ID3D11DeviceContext* ctx, bool force = false);

    // [B2] Log with tex format, rtv format, and dsv flag
    void LogUniqueRenderTargetLocked(uint32_t w,
                                     uint32_t h,
                                     uint32_t arraySize,
                                     DXGI_FORMAT texFmt,
                                     DXGI_FORMAT rtvFmt,
                                     uint32_t samples,
                                     D3D11_RTV_DIMENSION rtvDim,
                                     bool hasDsv,
                                     const char* resultLabel);

    void LogNvapiFailureRateLimited(const char* opName, NvAPI_Status status);

    std::mutex mutex_;
    std::atomic<bool> fastEnabled_{ false };
    std::atomic<bool> fastBound_{ false };

    bool nvapiChecked_ = false;
    bool nvapiSupported_ = false;
    bool hooksInstalled_ = false;
    bool hookInstallAttempted_ = false;

    bool enabled_ = false;
    int debugMode_ = 0;
    bool vrsCurrentlyBound_ = false;
    uint32_t targetEyeWidth_ = 0;
    uint32_t targetEyeHeight_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    std::vector<PatternEntry> patternCache_[static_cast<size_t>(VrsLayout::Count)];
    bool cacheFullLogged_ = false;

    // [B1] Non-owning excluded textures vector
    std::vector<ID3D11Texture2D*> excludedTextures_;

    std::unordered_set<uint64_t> loggedUniqueRts_;
    std::atomic<uint64_t> framesCount_{ 0 };
    std::atomic<uint64_t> candidateBinds_{ 0 };
    std::atomic<uint64_t> enableCount_{ 0 };
    std::atomic<uint64_t> disableCount_{ 0 };
    std::atomic<uint64_t> nvFailCount_{ 0 };
    uint32_t nvFailLogCount_ = 0;
};

class VrsSuppressScope {
public:
    VrsSuppressScope() noexcept { VrsFoveatedD3D11::EnterSuppressScope(); }
    ~VrsSuppressScope() noexcept { VrsFoveatedD3D11::LeaveSuppressScope(); }

    VrsSuppressScope(const VrsSuppressScope&) = delete;
    VrsSuppressScope& operator=(const VrsSuppressScope&) = delete;
};

} // namespace vrpacing

#endif // _WIN32