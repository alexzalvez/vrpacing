#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <string>

#include "nvapi.h"

namespace vrpacing {

enum class VrsTextureMode {
    SingleEye,
    CombinedSideBySide,
    TextureArray
};

class VrsFoveatedD3D11 {
public:
    static VrsFoveatedD3D11& Instance();

    void UpdateTarget(ID3D11Device* device,
                      ID3D11DeviceContext* context,
                      bool enabled,
                      uint32_t eyeWidth,
                      uint32_t eyeHeight,
                      VrsTextureMode mode,
                      float leftProjX = 0.5f,
                      float leftProjY = 0.5f,
                      float rightProjX = 0.5f,
                      float rightProjY = 0.5f);

    void EndFrame();
    void Shutdown();

    void OnPostOMSetRenderTargets(ID3D11DeviceContext* ctx,
                                  UINT numViews,
                                  ID3D11RenderTargetView* const* renderTargetViews);

private:
    VrsFoveatedD3D11() = default;
    ~VrsFoveatedD3D11() { Shutdown(); }

    bool EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void InstallContextHooksLocked(ID3D11DeviceContext* ctx);

    void EnableVRS(ID3D11DeviceContext* ctx);
    void DisableVRS(ID3D11DeviceContext* ctx);

    void ApplySingleEyeVRS(ID3D11DeviceContext* ctx, int eye, int width, int height);
    void ApplyCombinedVRS(ID3D11DeviceContext* ctx, int width, int height);
    void ApplyArrayVRS(ID3D11DeviceContext* ctx, int width, int height);

    void SetupSingleEyeVRS(int eye, int width, int height, float projX, float projY);
    void SetupCombinedVRS(int width, int height, float leftProjX, float leftProjY, float rightProjX, float rightProjY);
    void SetupArrayVRS(int width, int height, float leftProjX, float leftProjY, float rightProjX, float rightProjY);

    std::mutex mutex_;
    bool nvapiLoaded_ = false;
    bool nvapiSupported_ = false;
    bool hooksInstalled_ = false;
    bool enabled_ = false;
    bool vrsCurrentlyBound_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    int targetEyeWidth_ = 0;
    int targetEyeHeight_ = 0;
    VrsTextureMode targetMode_ = VrsTextureMode::SingleEye;
    float proj_[2][2] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };

    int singleWidth_[2] = { 0, 0 };
    int singleHeight_[2] = { 0, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> singleEyeVRSTex_[2];
    Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> singleEyeVRSView_[2];
    std::string singleEyeOrder_;
    size_t currentSingleEyeRT_ = 0;

    int combinedWidth_ = 0;
    int combinedHeight_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> combinedVRSTex_;
    Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> combinedVRSView_;

    int arrayWidth_ = 0;
    int arrayHeight_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> arrayVRSTex_;
    Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> arrayVRSView_;
};

} // namespace vrpacing

#endif