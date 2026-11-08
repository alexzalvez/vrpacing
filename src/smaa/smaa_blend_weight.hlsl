// smaa_blend_weight.hlsl
// SMAA Pass 2 - Orthogonal & 45-degree Diagonal Blend Weight Calculation (cs_5_0).

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

// -----------------------------------------------------------------------------
// Orthogonal Searches (Left, Right, Up, Down)
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// AreaTex Lookups
// -----------------------------------------------------------------------------

// Orthogonal lookup (left half of AreaTex: x = 0..79 texels)
float2 AreaOrthogonal(float2 dist, float e1, float e2) {
    float2 clampedDist = clamp(dist, 0.0f, 15.0f);
    float2 texcoord = k_areaTexMaxDistance * round(4.0f * float2(e1, e2)) + clampedDist;
    texcoord = (texcoord + 0.5f) * k_areaTexPixelSize;
    return g_areaTex.SampleLevel(g_linearSampler, texcoord, 0.0f).rg;
}

// Diagonal lookup (right half of AreaTex: x = 80..159 texels)
float2 AreaDiagonal(float2 dist, float2 e) {
    float2 clampedDist = clamp(dist, 0.0f, 19.0f);
    float2 texcoord = 20.0f * round(4.0f * e) + clampedDist;
    texcoord = (texcoord + 0.5f) * k_areaTexPixelSize;
    texcoord.x += 0.5f; // Jump to right half (80 texels offset)
    return g_areaTex.SampleLevel(g_linearSampler, texcoord, 0.0f).rg;
}

// -----------------------------------------------------------------------------
// 45-degree Diagonal Searches
// -----------------------------------------------------------------------------

// Diagonal 1: top-left to bottom-right (\)
float2 SearchDiag1(int2 ipos, int2 dims, out float2 e) {
    int d1 = 0;
    e = float2(0.0f, 0.0f);
    for (int i = 1; i <= 8; ++i) {
        int2 p = ipos - int2(i, i);
        if (p.x < 0 || p.y < 0) break;
        float2 edge = g_edgesTex[p];
        if (edge.r > 0.5f && edge.g > 0.5f) {
            d1 = i;
        } else {
            e.x = edge.r > 0.5f ? 0.25f : (edge.g > 0.5f ? 0.75f : 0.0f);
            break;
        }
    }

    int d2 = 0;
    for (int j = 1; j <= 8; ++j) {
        int2 p = ipos + int2(j, j);
        if (p.x >= dims.x || p.y >= dims.y) break;
        float2 edge = g_edgesTex[p];
        if (edge.r > 0.5f && edge.g > 0.5f) {
            d2 = j;
        } else {
            e.y = edge.g > 0.5f ? 0.25f : (edge.r > 0.5f ? 0.75f : 0.0f);
            break;
        }
    }
    return float2(float(d1), float(d2));
}

// Diagonal 2: bottom-left to top-right (/)
float2 SearchDiag2(int2 ipos, int2 dims, out float2 e) {
    int d1 = 0;
    e = float2(0.0f, 0.0f);
    for (int i = 1; i <= 8; ++i) {
        int2 p = ipos + int2(-i, i);
        if (p.x < 0 || p.y >= dims.y) break;
        float2 edge = g_edgesTex[p];
        if (edge.r > 0.5f && edge.g > 0.5f) {
            d1 = i;
        } else {
            e.x = edge.r > 0.5f ? 0.25f : (edge.g > 0.5f ? 0.75f : 0.0f);
            break;
        }
    }

    int d2 = 0;
    for (int j = 1; j <= 8; ++j) {
        int2 p = ipos + int2(j, -j);
        if (p.x >= dims.x || p.y < 0) break;
        float2 edge = g_edgesTex[p];
        if (edge.r > 0.5f && edge.g > 0.5f) {
            d2 = j;
        } else {
            e.y = edge.g > 0.5f ? 0.25f : (edge.r > 0.5f ? 0.75f : 0.0f);
            break;
        }
    }
    return float2(float(d1), float(d2));
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

    // -------------------------------------------------------------------------
    // 1. DIAGONAL EDGE HANDLING (45 degrees)
    // -------------------------------------------------------------------------
    bool hasDiag1 = (edges.r > 0.5f && edges.g > 0.5f);
    bool hasDiag2 = (edges.g > 0.5f && ReadEdgeClamped(ipos + int2(1, 0), dims).r > 0.5f);

    if (hasDiag1) {
        float2 eDiag;
        float2 dDiag = SearchDiag1(ipos, dims, eDiag);
        if ((dDiag.x + dDiag.y) >= 1.0f) {
            float2 w = AreaDiagonal(dDiag, eDiag);
            if (dot(w, float2(1.0f, 1.0f)) > 0.0f) {
                weights.rg = max(weights.rg, w);
            }
        }
    }

    if (hasDiag2) {
        float2 eDiag;
        float2 dDiag = SearchDiag2(ipos, dims, eDiag);
        if ((dDiag.x + dDiag.y) >= 1.0f) {
            float2 w = AreaDiagonal(dDiag, eDiag);
            if (dot(w, float2(1.0f, 1.0f)) > 0.0f) {
                weights.ba = max(weights.ba, w);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. ORTHOGONAL EDGE HANDLING (Horizontal & Vertical)
    // -------------------------------------------------------------------------
    // Top horizontal edge
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

        float2 w = AreaOrthogonal(float2(float(dLeft), float(dRight)), e1, e2);
        weights.rg = max(weights.rg, w);
    }

    // Left vertical edge
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

        float2 w = AreaOrthogonal(float2(float(dUp), float(dDown)), e1, e2);
        weights.ba = max(weights.ba, w);
    }

    g_blendTex[ipos] = weights;
}