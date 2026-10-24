// Copyright 2026 N-GINN LLC. All rights reserved.
// Separable bilateral Gaussian blur following Bogdanov M.K.'s SsaoBlur.hlsl.
#include "ssao_common.hlsli"

// Preserve the order of depth differences before normalization on either AO device.
precise float4 PSReconstructNormals(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float3 viewNormal = reconstructDepthNormal(int2(position.xy), int2(width, height));
    float3 worldNormal = normalize(mul(transpose((float3x3)aoView), viewNormal));
    // Preserve the existing sphere-map world-normal contract of the primary and secondary AO passes.
    if (worldNormal.z <= -0.999999)
    {
        return float4(1.0, 0.5, 0.0, 1.0);
    }
    float2 encoded = worldNormal.xy / sqrt(2.0 * (worldNormal.z + 1.0)) * 0.5 + 0.5;
    // Round explicitly before writing the same compact normal format on either device.
    return float4(f16tof32(f32tof16(encoded)), 0.0, 1.0);
}

float PSCopyDepth(float4 position : SV_Position) : SV_Target
{
    return aoDepth.Load(int3(int2(position.xy), 0));
}

float4 PSDebug(float4 position : SV_Position) : SV_Target
{
    float value = aoInput.Load(int3(int2(position.xy), 0));
    return float4(value.xxx, 1.0);
}

float PSMain(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float depth = aoDepth.Load(int3(pixel, 0));
    if (depth <= 0.0 || aoFilter.w == 0.0)
    {
        return 1.0;
    }
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float3 normal = readViewNormal(pixel);
    float viewZ = reconstructViewPosition(position.xy / size, depth).z;
    float total = aoInput.Load(int3(pixel, 0));
    float weightSum = 1.0;
    [unroll]
    for (int offset = -5; offset <= 5; ++offset)
    {
        int2 neighbor = pixel + int2(aoFilter.xy) * offset;
        if (offset == 0 || any(neighbor < 0) || any(neighbor >= int2(width, height)))
        {
            continue;
        }
        float neighborDepth = aoDepth.Load(int3(neighbor, 0));
        if (neighborDepth <= 0.0)
        {
            continue;
        }
        float neighborZ = reconstructViewPosition((float2(neighbor) + 0.5) / size, neighborDepth).z;
        if (dot(normal, readViewNormal(neighbor)) >= 0.8 && abs(neighborZ - viewZ) <= aoFilter.z)
        {
            float weight = exp(-float(offset * offset) / 12.5); // sigma = 2.5
            total += weight * aoInput.Load(int3(neighbor, 0));
            weightSum += weight;
        }
    }
    return saturate(total / weightSum);
}
