// smaa_neighborhood_blend.hlsl
// SMAA Pass 3 - 1D Dominant-Axis Blend with Symmetric Ridge & Opposing-Edge Text Protection (cs_5_0).

#include "smaa_constants.hlsli"

Texture2D<float4>   g_colorTex  : register(t0);
Texture2D<float4>   g_blendTex  : register(t1);
RWTexture2D<float4> g_outputTex : register(u0);

static const float3 k_luma = float3(0.2126f, 0.7152f, 0.0722f);

inline float Luma(float3 rgb) {
    return dot(rgb, k_luma);
}

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

    // a.xy = vertical blend weights (from horizontal edges)
    // a.zw = horizontal blend weights (from vertical edges)
    float4 a = float4(centerW.r, bottomG, centerW.b, rightA);
    if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f) {
        g_outputTex[basePos] = originalColor;
        return;
    }

    // Strict 1D dominant-axis selection
    bool horizEdgeDominant = (max(a.x, a.y) > max(a.z, a.w));

    float4 blended = originalColor;
    float lC = Luma(originalColor.rgb);

    if (horizEdgeDominant) {
        float w1 = a.x;
        float w2 = a.y;
        float wSum = w1 + w2;
        if (wSum > 1e-5f) {
            float4 cT = g_colorTex.Load(int3(clamp(basePos + int2(0, -1), minPos, maxPos), 0));
            float4 cB = g_colorTex.Load(int3(clamp(basePos + int2(0,  1), minPos, maxPos), 0));

            // --- FONT / HUD TEXT PROTECTION SHIELD ---
            // 1. Opposing edge dampening: real 3D geometry only has an edge on one side (w2 == 0).
            //    1-pixel font strokes have opposing edges on both sides (w1 > 0 and w2 > 0).
            float opposing = min(w1, w2);
            float protectOpposing = saturate(1.0f - 4.0f * opposing);

            // 2. Symmetric luminance ridge test (CMAA2-style):
            //    If opposite background pixels (cT, cB) are similar, but center stands out sharply,
            //    this is an isolated font stroke or reticle, not a continuous polygon gradient.
            float lT = Luma(cT.rgb);
            float lB = Luma(cB.rgb);
            float oppDiff = abs(lT - lB);
            float ctrDiff = min(abs(lC - lT), abs(lC - lB));
            float protectRidge = (ctrDiff > (2.0f * oppDiff + 0.08f)) ? 0.0f : 1.0f;

            float effFactor = protectOpposing * protectRidge;
            w1 *= effFactor;
            w2 *= effFactor;
            float newSum = w1 + w2;

            if (newSum > 1e-5f) {
                float4 blendT = lerp(originalColor, cT, w1);
                float4 blendB = lerp(originalColor, cB, w2);
                blended = (w1 * blendT + w2 * blendB) / newSum;
            }
        }
    } else {
        float w1 = a.z;
        float w2 = a.w;
        float wSum = w1 + w2;
        if (wSum > 1e-5f) {
            float4 cL = g_colorTex.Load(int3(clamp(basePos + int2(-1, 0), minPos, maxPos), 0));
            float4 cR = g_colorTex.Load(int3(clamp(basePos + int2( 1, 0), minPos, maxPos), 0));

            // --- FONT / HUD TEXT PROTECTION SHIELD ---
            // 1. Opposing edge dampening
            float opposing = min(w1, w2);
            float protectOpposing = saturate(1.0f - 4.0f * opposing);

            // 2. Symmetric luminance ridge test
            float lL = Luma(cL.rgb);
            float lR = Luma(cR.rgb);
            float oppDiff = abs(lL - lR);
            float ctrDiff = min(abs(lC - lL), abs(lC - lR));
            float protectRidge = (ctrDiff > (2.0f * oppDiff + 0.08f)) ? 0.0f : 1.0f;

            float effFactor = protectOpposing * protectRidge;
            w1 *= effFactor;
            w2 *= effFactor;
            float newSum = w1 + w2;

            if (newSum > 1e-5f) {
                float4 blendL = lerp(originalColor, cL, w1);
                float4 blendR = lerp(originalColor, cR, w2);
                blended = (w1 * blendL + w2 * blendR) / newSum;
            }
        }
    }

    blended.a = originalColor.a;
    g_outputTex[basePos] = blended;
}