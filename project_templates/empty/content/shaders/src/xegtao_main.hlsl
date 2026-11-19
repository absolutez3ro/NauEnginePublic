// Copyright 2026 N-GINN LLC. All rights reserved.
#include "xegtao_common.hlsli"

Texture2D<float2> xeSourceNormals : register(t0);
Texture2D<float> xeWorkingDepth : register(t2);
RWTexture2D<uint> xeWorkingAo : register(u0);
RWTexture2D<unorm float> xeEdges : register(u1);

float3 xeDecodeNormal(float2 encoded)
{
    if (aoOptions.y != 0.0)
    {
        float2 f = encoded * 2.0 - 1.0;
        float3 normal = float3(f, 1.0 - abs(f.x) - abs(f.y));
        if (normal.z < 0.0)
            normal.xy = (1.0 - abs(normal.yx)) * float2(normal.x >= 0.0 ? 1.0 : -1.0, normal.y >= 0.0 ? 1.0 : -1.0);
        return normalize(normal) * float3(1.0, 1.0, -1.0);
    }
    float2 f = encoded * 2.0 - 1.0;
    return normalize(float3(2.0 * f * sqrt(max(0.0, 1.0 - dot(f, f))), 1.0 - 2.0 * dot(f, f))) * float3(1.0, 1.0, -1.0);
}

float3 xeReconstructNormal(uint2 pixel, GTAOConstants constants)
{
    float2 uv = (pixel + 0.5) * constants.ViewportPixelSize;
    float4 upper = xeWorkingDepth.GatherRed(xePointClamp, pixel * constants.ViewportPixelSize);
    float4 lower = xeWorkingDepth.GatherRed(xePointClamp, pixel * constants.ViewportPixelSize, int2(1, 1));
    float center = upper.y;
    float4 edges = XeGTAO_CalculateEdges(center, upper.x, lower.z, upper.z, lower.x);
    float3 c = XeGTAO_ComputeViewspacePosition(uv, center, constants);
    float3 l = XeGTAO_ComputeViewspacePosition(uv + float2(-1, 0) * constants.ViewportPixelSize, upper.x, constants);
    float3 r = XeGTAO_ComputeViewspacePosition(uv + float2(1, 0) * constants.ViewportPixelSize, lower.z, constants);
    float3 t = XeGTAO_ComputeViewspacePosition(uv + float2(0, -1) * constants.ViewportPixelSize, upper.z, constants);
    float3 b = XeGTAO_ComputeViewspacePosition(uv + float2(0, 1) * constants.ViewportPixelSize, lower.x, constants);
    return XeGTAO_CalculateNormal(edges, c, l, r, t, b);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= uint2(aoSizes.xy))) return;
    GTAOConstants constants = loadXeGtaoConstants();
    float3 normal;
    if (aoOptions.x != 0.0) normal = xeReconstructNormal(pixel, constants);
    else
    {
        int2 inputPixel = min(int2((pixel + 0.5) * aoSizes.zw / aoSizes.xy), int2(aoSizes.zw) - 1);
        normal = xeDecodeNormal(xeSourceNormals.Load(int3(inputPixel, 0)));
    }
    // XeGTAO's Hilbert/R2 noise keeps the index within a 64x64 tile.
    // Large linear pixel indices lose their fractional bits in float32 and form bands.
    uint index = HilbertIndex(pixel.x % XE_HILBERT_WIDTH, pixel.y % XE_HILBERT_WIDTH);
    float2 noise = frac(0.5 + float(index) * float2(0.75487766624669276, 0.56984029099805326));
    XeGTAO_MainPass(pixel, 3, 3, noise, normal, constants,
        xeWorkingDepth, xePointClamp, xeWorkingAo, xeEdges);
}
