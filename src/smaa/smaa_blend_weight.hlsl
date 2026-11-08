// smaa_blend_weight.hlsl
// SMAA Pass 2 - Linear AreaTex analytical lookup without destructive span clipping (cs_5_0).

#include "smaa_constants.hlsli"

Texture2D<float2>   g_edgesTex  : register(t0);
Texture2D<float4>   g_areaTex   : register(t1);
Texture2D<float>    g_searchTex : register(t2);

RWTexture2D<float4> g_blendTex  : register(u0);

SamplerState g_linearSampler : register(s0);
SamplerState g_pointSampler  : register(s1);

static const float2 k_areaTexMaxDistance = float2(16.0f, 16.0f);
static const float2 k_areaTexPixelSize   = 1.0f / float2(160.0f, 560.0f);

float2 ReadEdgeClamped(int2 p, int2 dims) {
    if (p.x < 0 || p.y < 0 || p.x >= dims.x || p.y >= dims.y) {
        return float2(0.0f, 0.0f);
    }
    return g_edgesTex[p];
}

int SearchHorizontalLeft(int2 ipos, int2 dims) {
    int d = 0;
    for (int i = 0; i < g_maxSearchSteps; ++i) {
        if (ReadEdgeClamped(int2(ipos.x - d, ipos.y), dims).r > 0.5f ||
            ReadEdgeClamped(int2(ipos.x - d, ipos.y - 1), dims).r > 0.5f) {
            break;
        }
        int nextX = ipos.x - d - 1;
        if (nextX < 0) break;
        if (ReadEdgeClamped(int2(nextX, ipos.y), dims).g < 0.5f) break;
        d++;
    }
    return d;
}

int SearchHorizontalRight(int2 ipos, int2 dims) {
    int d = 0;
    for (int i = 0; i < g_maxSearchSteps; ++i) {
        if (ReadEdgeClamped(int2(ipos.x + d + 1, ipos.y), dims).r > 0.5f ||
            ReadEdgeClamped(int2(ipos.x + d + 1, ipos.y - 1), dims).r > 0.5f) {
            break;
        }
        int nextX = ipos.x + d + 1;
        if (nextX >= dims.x) break;
        if (ReadEdgeClamped(int2(nextX, ipos.y), dims).g < 0.5f) break;
        d++;
    }
    return d;
}

int SearchVerticalUp(int2 ipos, int2 dims) {
    int d = 0;
    for (int i = 0; i < g_maxSearchSteps; ++i) {
        if (ReadEdgeClamped(int2(ipos.x, ipos.y - d), dims).g > 0.5f ||
            ReadEdgeClamped(int2(ipos.x - 1, ipos.y - d), dims).g > 0.5f) {
            break;
        }
        int nextY = ipos.y - d - 1;
        if (nextY < 0) break;
        if (ReadEdgeClamped(int2(ipos.x, nextY), dims).r < 0.5f) break;
        d++;
    }
    return d;
}

int SearchVerticalDown(int2 ipos, int2 dims) {
    int d = 0;
    for (int i = 0; i < g_maxSearchSteps; ++i) {
        if (ReadEdgeClamped(int2(ipos.x, ipos.y + d + 1), dims).g > 0.5f ||
            ReadEdgeClamped(int2(ipos.x - 1, ipos.y + d + 1), dims).g > 0.5f) {
            break;
        }
        int nextY = ipos.y + d + 1;
        if (nextY >= dims.y) break;
        if (ReadEdgeClamped(int2(ipos.x, nextY), dims).r < 0.5f) break;
        d++;
    }
    return d;
}

// Exact linear AreaTex analytical coordinates (Jorge Jimenez specification)
float2 Area(float2 dist, float e1, float e2) {
    float2 texcoord = k_areaTexMaxDistance * round(4.0f * float2(e1, e2)) + round(dist);
    texcoord = (texcoord + 0.5f) * k_areaTexPixelSize;
    return g_areaTex.SampleLevel(g_linearSampler, texcoord, 0.0f).rg;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    int2 ipos = int2(dtid.xy);
    int2 dims = int2(g_rtMetrics.zw);
    if (any(ipos >= dims)) {
        return;
    }

    float2 edges = g_edgesTex[ipos];
    if (!any(edges > 0.5f)) {
        g_blendTex[ipos] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 weights = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // 1. HORIZONTAL edge on pixel top boundary (edges.g > 0.5)
    if (edges.g > 0.5f) {
        int dLeft  = SearchHorizontalLeft(ipos, dims);
        int dRight = SearchHorizontalRight(ipos, dims);

        int leftCol  = ipos.x - dLeft;
        int rightCol = ipos.x + dRight + 1;

        float e1Top = ReadEdgeClamped(int2(leftCol,  ipos.y - 1), dims).r;
        float e1Bot = ReadEdgeClamped(int2(leftCol,  ipos.y),     dims).r;
        float e1    = 0.25f * e1Top + 0.75f * e1Bot;

        float e2Top = ReadEdgeClamped(int2(rightCol, ipos.y - 1), dims).r;
        float e2Bot = ReadEdgeClamped(int2(rightCol, ipos.y),     dims).r;
        float e2    = 0.25f * e2Top + 0.75f * e2Bot;

        float2 w = Area(float2(dLeft, dRight), e1, e2);
        weights.rg = w;
    }

    // 2. VERTICAL edge on pixel left boundary (edges.r > 0.5)
    if (edges.r > 0.5f) {
        int dUp   = SearchVerticalUp(ipos, dims);
        int dDown = SearchVerticalDown(ipos, dims);

        int topRow    = ipos.y - dUp;
        int bottomRow = ipos.y + dDown + 1;

        float e1Left  = ReadEdgeClamped(int2(ipos.x - 1, topRow), dims).g;
        float e1Right = ReadEdgeClamped(int2(ipos.x,     topRow), dims).g;
        float e1      = 0.25f * e1Left + 0.75f * e1Right;

        float e2Left  = ReadEdgeClamped(int2(ipos.x - 1, bottomRow), dims).g;
        float e2Right = ReadEdgeClamped(int2(ipos.x,     bottomRow), dims).g;
        float e2      = 0.25f * e2Left + 0.75f * e2Right;

        float2 w = Area(float2(dUp, dDown), e1, e2);
        weights.ba = w;
    }

    g_blendTex[ipos] = weights;
}