#ifdef _WIN32

#include "nis_upscaler_d3d11.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "common/logging_win.h"

namespace vrpacing {

bool NisUpscalerD3D11::Initialize(ID3D11Device* device) {
    device_ = device;
    if (!device_) {
        return false;
    }

    LogMessage(LogLevel::Info, "NIS D3D11 inicializado.");
    return true;
}

bool NisUpscalerD3D11::UpscaleStereo(ID3D11DeviceContext* ctx,
                                     ID3D11Texture2D* src,
                                     ID3D11Texture2D* dst,
                                     const UpscaleDispatchDesc& desc) {
    if (!device_ || !ctx || !src || !dst) {
        return false;
    }

    D3D11_TEXTURE2D_DESC outDesc{};
    dst->GetDesc(&outDesc);
    const uint64_t totalPixels = static_cast<uint64_t>(outDesc.Width) * static_cast<uint64_t>(outDesc.Height);
    if (totalPixels > desc.maxOutputPixels) {
        LogMessage(LogLevel::Warn, "NIS omitido por límite de píxeles de salida.");
        return false;
    }

    ctx->CopyResource(dst, src);
    return true;
}

} // namespace vrpacing

#endif
