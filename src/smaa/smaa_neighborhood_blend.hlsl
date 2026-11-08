// smaa_neighborhood_blend.hlsl
// SMAA Pass 3 - Mezcla final de vecindad con conservacion exacta de pesos (Compute Shader cs_5_0).

#include "smaa_constants.hlsli"

Texture2D<float4>   g_colorTex  : register(t0);
Texture2D<float4>   g_blendTex  : register(t1);
RWTexture2D<float4> g_outputTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    int2 dims = int2(g_rtMetrics.zw);
    int2 localPos = int2(dtid.xy);
    if (any(localPos >= dims)) {
        return;
    }

    int2 basePos = int2(g_viewportOffset) + localPos;
    int2 minPos  = int2(g_viewportOffset);
    int2 maxPos  = int2(g_viewportOffset) + dims - 1;

    float4 originalColor = g_colorTex.Load(int3(basePos, 0));

    float2 uv = (float2(localPos) + 0.5f) * g_rtMetrics.xy;
    if (InFovealMask(uv) < 0.5f) {
        g_outputTex[basePos] = originalColor;
        return;
    }

    // Lectura alineada con la convencion de Pass 2:
    // .r = mezcla hacia arriba  (en el propio pixel)
    // .g = mezcla hacia abajo   (en el vecino inferior)
    // .b = mezcla hacia izq     (en el propio pixel)
    // .a = mezcla hacia dcha    (en el vecino derecho)
    float4 centerW = g_blendTex.Load(int3(localPos, 0));
    float  bottomG = (localPos.y + 1 < dims.y) ? g_blendTex.Load(int3(localPos + int2(0, 1), 0)).g : 0.0f;
    float  rightA  = (localPos.x + 1 < dims.x) ? g_blendTex.Load(int3(localPos + int2(1, 0), 0)).a : 0.0f;

    float4 a = float4(centerW.r, bottomG, centerW.b, rightA);
    float sum = dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f));

    if (sum < 1e-5f) {
        g_outputTex[basePos] = originalColor;
        return;
    }

    float4 cT = g_colorTex.Load(int3(clamp(basePos + int2( 0, -1), minPos, maxPos), 0));
    float4 cB = g_colorTex.Load(int3(clamp(basePos + int2( 0,  1), minPos, maxPos), 0));
    float4 cL = g_colorTex.Load(int3(clamp(basePos + int2(-1,  0), minPos, maxPos), 0));
    float4 cR = g_colorTex.Load(int3(clamp(basePos + int2( 1,  0), minPos, maxPos), 0));

    float4 w = (sum > 1.0f) ? (a / sum) : a;
    float centerWeight = max(0.0f, 1.0f - sum);

    float4 blended = centerWeight * originalColor + w.x * cT + w.y * cB + w.z * cL + w.w * cR;
    blended.a = originalColor.a;

    g_outputTex[basePos] = blended;
}
