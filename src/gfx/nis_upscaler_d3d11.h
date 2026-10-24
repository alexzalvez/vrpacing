#pragma once

#ifdef _WIN32

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace vrpacing {

struct UpscaleDispatchDesc {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float radius = 0.60f;
    float sharpness = 0.0f;
    bool maskEnabled = true;
    uint64_t maxOutputPixels = 32000000ull;
};

class NisUpscalerD3D11 {
public:
    bool Initialize(ID3D11Device* device);
    bool UpscaleStereo(ID3D11DeviceContext* ctx,
                       ID3D11Texture2D* src,
                       ID3D11Texture2D* dst,
                       const UpscaleDispatchDesc& desc);

private:
    ID3D11Device* device_ = nullptr;
};

} // namespace vrpacing

#endif
