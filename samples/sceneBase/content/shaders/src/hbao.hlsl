// Copyright 2026 N-GINN LLC. All rights reserved.
#include "hbao_common.hlsli"

// R8_UNORM UAV supplied by the native GPU1 pass; no extra constants or normal SRV.
RWTexture2D<float> hbaoOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= uint2(aoSizes.xy))) return;
    // All in-bounds threads write, including sky, radius-zero and subpixel cases.
    hbaoOutput[dispatchId.xy] = computeHbao(dispatchId.xy);
}
