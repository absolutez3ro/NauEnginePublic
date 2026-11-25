// Copyright 2026 N-GINN LLC. All rights reserved.
#include "xegtao_common.hlsli"

Texture2D<float2> xeSourceNormals : register(t0);
Texture2D<float> xeWorkingDepth : register(t2);
RWTexture2D<uint> xeWorkingAo : register(u0);
RWTexture2D<unorm float> xeEdges : register(u1);

float3 xeDecodeNormal(float2 encoded)
{
    float2 f = encoded * 2.0 - 1.0;
    return normalize(float3(2.0 * f * sqrt(max(0.0, 1.0 - dot(f, f))), 1.0 - 2.0 * dot(f, f))) * float3(1.0, 1.0, -1.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= uint2(aoSizes.xy))) return;
    GTAOConstants constants = loadXeGtaoConstants();
    float3 normal = xeDecodeNormal(xeSourceNormals.Load(int3(pixel, 0)));
    // XeGTAO's Hilbert/R2 noise keeps the index within a 64x64 tile.
    // Large linear pixel indices lose their fractional bits in float32 and form bands.
    uint index = HilbertIndex(pixel.x % XE_HILBERT_WIDTH, pixel.y % XE_HILBERT_WIDTH);
    float2 noise = frac(0.5 + float(index) * float2(0.75487766624669276, 0.56984029099805326));
    XeGTAO_MainPass(pixel, 3, 3, noise, normal, constants,
        xeWorkingDepth, xePointClamp, xeWorkingAo, xeEdges);
}
