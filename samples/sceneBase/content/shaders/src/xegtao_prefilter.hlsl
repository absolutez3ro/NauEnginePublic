// Copyright 2026 N-GINN LLC. All rights reserved.
#include "xegtao_common.hlsli"

Texture2D<float> xeSourceDepth : register(t1);
RWTexture2D<float> xeDepthMip0 : register(u0);
RWTexture2D<float> xeDepthMip1 : register(u1);
RWTexture2D<float> xeDepthMip2 : register(u2);
RWTexture2D<float> xeDepthMip3 : register(u3);
RWTexture2D<float> xeDepthMip4 : register(u4);

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchId : SV_DispatchThreadID, uint2 groupId : SV_GroupThreadID)
{
    // Every lane must participate in the prefilter's group barriers.
    XeGTAO_PrefilterDepths16x16(dispatchId, groupId, loadXeGtaoConstants(),
        xeSourceDepth, xePointClamp, xeDepthMip0, xeDepthMip1, xeDepthMip2, xeDepthMip3, xeDepthMip4);
}
