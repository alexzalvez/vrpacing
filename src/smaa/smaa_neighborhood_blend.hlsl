// smaa_neighborhood_blend.hlsl
// SMAA Pass 3 - 1D Dominant-Axis with Opposing-Edge Font/Text Protection (cs_5_0).

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

    float4 centerW = g_blendTex.Load(int3(localPos, 0));
    float  bottomG = (localPos.y + 1 < dims.y) ? g_blendTex.Load(int3(localPos + int2(0, 1), 0)).g : 0.0f;
    float  rightA  = (localPos.x + 1 < dims.x) ? g_blendTex.Load(int3(localPos + int2(1, 0), 0)).a : 0.0f;

    // a.xy = vertical blend weights (from horizontal edges: top, bottom)
    // a.zw = horizontal blend weights (from vertical edges: left, right)
    float4 a = float4(centerW.r, bottomG, centerW.b, rightA);
    if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f) {
        g_outputTex[basePos] = originalColor;
        return;
    }

    // Strict 1D dominant-axis selection
    bool horizEdgeDominant = (max(a.x, a.y) > max(a.z, a.w));

    float4 blended = originalColor;
    if (horizEdgeDominant) {
        float w1 = a.x;
        float w2 = a.y;

        // Proteccion de texto fino: si hay borde opuesto simultaneo (trazo de 1 px)
        // se atenua la mezcla para evitar borrar la letra.
        // En una arista geometrica normal w2 es 0, por lo que protect = 1.0 (100% AA).
        float opposing = min(w1, w2);
        float protect = saturate(1.0f - 2.0f * opposing);
        float eff_w1 = w1 * protect;
        float eff_w2 = w2 * protect;

        float wSum = eff_w1 + eff_w2;
        if (wSum > 1e-5f) {
            float4 cT = g_colorTex.Load(int3(clamp(basePos + int2(0, -1), minPos, maxPos), 0));
            float4 cB = g_colorTex.Load(int3(clamp(basePos + int2(0,  1), minPos, maxPos), 0));
            float4 blendT = lerp(originalColor, cT, eff_w1);
            float4 blendB = lerp(originalColor, cB, eff_w2);
            blended = (eff_w1 * blendT + eff_w2 * blendB) / wSum;
        }
    } else {
        float w1 = a.z;
        float w2 = a.w;

        float opposing = min(w1, w2);
        float protect = saturate(1.0f - 2.0f * opposing);
        float eff_w1 = w1 * protect;
        float eff_w2 = w2 * protect;

        float wSum = eff_w1 + eff_w2;
        if (wSum > 1e-5f) {
            float4 cL = g_colorTex.Load(int3(clamp(basePos + int2(-1, 0), minPos, maxPos), 0));
            float4 cR = g_colorTex.Load(int3(clamp(basePos + int2( 1, 0), minPos, maxPos), 0));
            float4 blendL = lerp(originalColor, cL, eff_w1);
            float4 blendR = lerp(originalColor, cR, eff_w2);
            blended = (eff_w1 * blendL + eff_w2 * blendR) / wSum;
        }
    }

    blended.a = originalColor.a;
    g_outputTex[basePos] = blended;
}