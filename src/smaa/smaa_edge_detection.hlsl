// smaa_edge_detection.hlsl
// SMAA Pass 1 - Hybrid Luma + Color edge detection (Compute Shader cs_5_0).

#include "smaa_constants.hlsli"

Texture2D<float4>   g_colorTex : register(t0);
RWTexture2D<float2> g_edgesTex : register(u0);

static const float3 k_luma = float3(0.2126f, 0.7152f, 0.0722f);

inline float EdgeDelta(float3 a, float3 b) {
    float3 d = abs(a - b);
    float lumaDiff = abs(dot(a, k_luma) - dot(b, k_luma));
    float chromaDiff = max(max(d.r, d.g), d.b);
    return max(lumaDiff, 0.5f * chromaDiff);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    int2 dims = int2(g_rtMetrics.zw);
    int2 localPos = int2(dtid.xy);
    if (any(localPos >= dims)) {
        return;
    }

    float2 uv = (float2(localPos) + 0.5f) * g_rtMetrics.xy;
    if (InFovealMask(uv) < 0.5f) {
        g_edgesTex[localPos] = float2(0.0f, 0.0f);
        return;
    }

    int2 basePos = int2(g_viewportOffset) + localPos;
    int2 minPos  = int2(g_viewportOffset);
    int2 maxPos  = int2(g_viewportOffset) + dims - 1;

    float3 cC  = g_colorTex.Load(int3(basePos, 0)).rgb;
    float3 cL  = g_colorTex.Load(int3(clamp(basePos + int2(-1,  0), minPos, maxPos), 0)).rgb;
    float3 cT  = g_colorTex.Load(int3(clamp(basePos + int2( 0, -1), minPos, maxPos), 0)).rgb;
    float3 cR  = g_colorTex.Load(int3(clamp(basePos + int2( 1,  0), minPos, maxPos), 0)).rgb;
    float3 cB  = g_colorTex.Load(int3(clamp(basePos + int2( 0,  1), minPos, maxPos), 0)).rgb;
    float3 cLL = g_colorTex.Load(int3(clamp(basePos + int2(-2,  0), minPos, maxPos), 0)).rgb;
    float3 cTT = g_colorTex.Load(int3(clamp(basePos + int2( 0, -2), minPos, maxPos), 0)).rgb;

    float deltaL = EdgeDelta(cC, cL);
    float deltaT = EdgeDelta(cC, cT);

    bool2 edges = bool2(deltaL >= g_threshold, deltaT >= g_threshold);
    if (!any(edges)) {
        g_edgesTex[localPos] = float2(0.0f, 0.0f);
        return;
    }

    float deltaR  = EdgeDelta(cC, cR);
    float deltaB  = EdgeDelta(cC, cB);
    float deltaLL = EdgeDelta(cL, cLL);
    float deltaTT = EdgeDelta(cT, cTT);

    float maxDelta = max(max(deltaL, deltaT), max(max(deltaR, deltaB), max(deltaLL, deltaTT)));
    float adaptiveThreshold = max(g_threshold, 0.5f * maxDelta);

    float2 outEdges;
    outEdges.x = (deltaL >= adaptiveThreshold) ? 1.0f : 0.0f; // Left vertical edge (.r)
    outEdges.y = (deltaT >= adaptiveThreshold) ? 1.0f : 0.0f; // Top horizontal edge (.g)

    g_edgesTex[localPos] = outEdges;
}