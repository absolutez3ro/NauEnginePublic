// Copyright 2026 N-GINN LLC. All rights reserved.
#include "xegtao_common.hlsli"

Texture2D<uint> xeWorkingAo : register(t2);
Texture2D<float> xeEdges : register(t3);
RWTexture2D<uint> xeDenoisedAo : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId * uint2(2, 1);
    if (any(pixel >= uint2(aoSizes.xy))) return;
    XeGTAO_Denoise(pixel, loadXeGtaoConstants(), xeWorkingAo, xeEdges, xePointClamp, xeDenoisedAo, false);
}

[numthreads(8, 8, 1)]
void CSLast(uint2 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId * uint2(2, 1);
    if (any(pixel >= uint2(aoSizes.xy))) return;
    XeGTAO_Denoise(pixel, loadXeGtaoConstants(), xeWorkingAo, xeEdges, xePointClamp, xeDenoisedAo, true);
}
