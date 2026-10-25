// The MIT License(MIT)
//
// Copyright(c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#define NIS_SCALER 1
#define NIS_HDR_MODE 0
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 24
#define NIS_THREAD_GROUP_SIZE 256
#define NIS_VIEWPORT_SUPPORT 1

Texture2D coef_scaler            : register(t1);
Texture2D coef_usm               : register(t2);

#include "NIS_Common.h"
#include "NIS_Scaler.h"

[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    int2 groupCentre = int2((blockIdx.x * 32) + 16, (blockIdx.y * 24) + 12);
    int2 dc = int2(projCentre.xy) - groupCentre;
    uint dSq = (uint)dot(dc, dc);

    if (dSq <= squaredRadius) {
        NVScaler(blockIdx.xy, threadIdx.x);
    }
    else {
        FillBlack(blockIdx.xy, threadIdx.x);
    }
}