// smaa_blend_weight.hlsl
// SMAA Pass 2 - Official SMAA 1x Ultra Blend Weight Calculation (cs_5_0).

#include "smaa_constants.hlsli"

Texture2D<float2>   g_edgesTex  : register(t0);
Texture2D<float4>   g_areaTex   : register(t1);
Texture2D<float>    g_searchTex : register(t2);

RWTexture2D<float4> g_blendTex  : register(u0);

SamplerState g_linearSampler : register(s0);
SamplerState g_pointSampler  : register(s1);

static const float2 k_areaTexMaxDistance     = float2(16.0f, 16.0f);
static const float2 k_areaTexMaxDistanceDiag = float2(20.0f, 20.0f);
static const float2 k_areaTexPixelSize       = 1.0f / float2(160.0f, 560.0f);
static const int    k_maxSearchStepsDiag     = 16;
static const float  k_cornerRounding         = 0.25f;

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

float2 AreaOrthogonal(float2 dist, float e1, float e2) {
    float2 sqrtDist = min(sqrt(round(dist)), 15.0f);
    float2 texcoord = k_areaTexMaxDistance * round(4.0f * float2(e1, e2)) + sqrtDist;
    texcoord = (texcoord + 0.5f) * k_areaTexPixelSize;
    return g_areaTex.SampleLevel(g_linearSampler, texcoord, 0.0f).rg;
}

float2 AreaDiagonal(float2 dist, float2 e) {
    float2 clampedDist = clamp(round(dist), 0.0f, 19.0f);
    float2 texcoord = k_areaTexMaxDistanceDiag * e + clampedDist;
    texcoord = (texcoord + 0.5f) * k_areaTexPixelSize;
    texcoord.x += 0.5f;
    return g_areaTex.SampleLevel(g_linearSampler, texcoord, 0.0f).rg;
}

float2 ApplyCornerRounding(float2 w, float e1, float e2) {
    bool isUCorner = ((e1 == 0.25f && e2 == 0.25f) || (e1 == 0.75f && e2 == 0.75f));
    if (isUCorner) {
        return w * k_cornerRounding;
    }
    return w;
}

float2 SearchDiag1(int2 ipos, int2 dims, int2 dir, out float2 endEdge) {
    int2 cur = ipos;
    int d = 0;
    endEdge = float2(0.0f, 0.0f);
    for (int i = 0; i < k_maxSearchStepsDiag; ++i) {
        cur += dir;
        if (cur.x < 0 || cur.y < 0 || cur.x >= dims.x || cur.y >= dims.y) break;
        float2 e = g_edgesTex[cur];
        endEdge = e;
        if (e.r > 0.5f && e.g > 0.5f) {
            d++;
        } else {
            break;
        }
    }
    return float2(float(d), (endEdge.r > 0.5f || endEdge.g > 0.5f) ? 1.0f : 0.0f);
}

float2 SearchDiag2(int2 ipos, int2 dims, int2 dir, out float2 endEdge) {
    int2 cur = ipos;
    int d = 0;
    endEdge = float2(0.0f, 0.0f);
    for (int i = 0; i < k_maxSearchStepsDiag; ++i) {
        cur += dir;
        if (cur.x < 0 || cur.y < 0 || cur.x >= dims.x || cur.y >= dims.y) break;
        float gEdge = g_edgesTex[cur].g;
        float rEdge = ReadEdgeClamped(cur + int2(1, 0), dims).r;
        endEdge = float2(rEdge, gEdge);
        if (rEdge > 0.5f && gEdge > 0.5f) {
            d++;
        } else {
            break;
        }
    }
    return float2(float(d), (endEdge.r > 0.5f || endEdge.g > 0.5f) ? 1.0f : 0.0f);
}

float2 CalculateDiagWeights(int2 ipos, int2 dims, float2 edges) {
    float2 weights = float2(0.0f, 0.0f);

    if (edges.r > 0.5f) {
        float2 end1, end2;
        float2 d1 = SearchDiag1(ipos, dims, int2(-1, -1), end1);
        float2 d2 = SearchDiag1(ipos, dims, int2( 1,  1), end2);
        float2 d  = float2(d1.x, d2.x);

        if ((d.x + d.y) > 2.0f) {
            float4 e = float4(
                (d1.y > 0.5f) ? end1.g : 0.0f,
                (d1.y > 0.5f) ? end1.r : 0.0f,
                (d2.y > 0.5f) ? end2.r : 0.0f,
                (d2.y > 0.5f) ? end2.g : 0.0f
            );
            float2 c = float2(2.0f * e.x + e.y, 2.0f * e.z + e.w);
            weights += AreaDiagonal(d, c);
        }
    }

    float rightR = ReadEdgeClamped(ipos + int2(1, 0), dims).r;
    if (rightR > 0.5f) {
        float2 end1, end2;
        float2 d1 = SearchDiag2(ipos, dims, int2(-1,  1), end1);
        float2 d2 = SearchDiag2(ipos, dims, int2( 1, -1), end2);
        float2 d  = float2(d1.x, d2.x);

        if ((d.x + d.y) > 2.0f) {
            float4 e = float4(
                (d1.y > 0.5f) ? end1.g : 0.0f,
                (d1.y > 0.5f) ? end1.r : 0.0f,
                (d2.y > 0.5f) ? end2.r : 0.0f,
                (d2.y > 0.5f) ? end2.g : 0.0f
            );
            float2 c = float2(2.0f * e.x + e.y, 2.0f * e.z + e.w);
            weights += AreaDiagonal(d, c).gr;
        }
    }

    return weights;
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

    if (edges.g > 0.5f) {
        weights.rg = CalculateDiagWeights(ipos, dims, edges);

        if (dot(weights.rg, float2(1.0f, 1.0f)) == 0.0f) {
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
            weights.rg = ApplyCornerRounding(w, e1, e2);
        }
    }

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
        weights.ba = ApplyCornerRounding(w, e1, e2);
    }

    g_blendTex[ipos] = weights;
}