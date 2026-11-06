// Copyright 2026 N-GINN LLC. All rights reserved.
#include "xegtao_common.hlsli"

Texture2D<float> xeSourceDepth : register(t1);
Texture2D<uint> xeDenoisedAo : register(t2);
RWTexture2D<unorm float> xeOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= uint2(aoSizes.xy))) return;
    float2 uv = (pixel + 0.5) / aoSizes.xy;
    float depth = xeSourceDepth.SampleLevel(xePointClamp, uv, 0);
    // SharedXeGTAO applies FinalValuePower once in MainPass and once in Composite.
    float visibility = xeDenoisedAo.Load(int3(pixel, 0)) / 255.05;
    xeOutput[pixel] = depth > 0.0 && aoFilter.w != 0.0 && aoParameters.x > 0.0
        ? saturate(pow(visibility, XE_GTAO_DEFAULT_FINAL_VALUE_POWER)) : 1.0;
}
