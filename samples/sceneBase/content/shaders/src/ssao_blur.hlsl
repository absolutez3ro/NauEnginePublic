// Copyright 2026 N-GINN LLC. All rights reserved.
// Separable bilateral Gaussian blur following Bogdanov M.K.'s SsaoBlur.hlsl.
#include "ssao_common.hlsli"

precise float4 PSReconstructNormals(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float3 viewNormal = reconstructDepthNormal(int2(position.xy), int2(width, height));
    float3 worldNormal = normalize(mul(transpose((float3x3)aoView), viewNormal));
    // Sphere-map encoding matches the G-buffer normal contract.
    if (worldNormal.z <= -0.999999)
    {
        return float4(1.0, 0.5, 0.0, 1.0);
    }
    float2 encoded = worldNormal.xy / sqrt(2.0 * (worldNormal.z + 1.0)) * 0.5 + 0.5;
    // Match RG16_FLOAT rounding on both devices.
    return float4(f16tof32(f32tof16(encoded)), 0.0, 1.0);
}

float PSCopyDepth(float4 position : SV_Position) : SV_Target
{
    return aoDepth.Load(int3(int2(position.xy), 0));
}

float PSDownsampleDepth(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoFullDepth.GetDimensions(width, height);
    uint2 outputSize = max(uint2(width, height) / uint(aoFilter.w), 1u);
    uint2 pixel = uint2(position.xy);
    uint2 first = pixel * uint2(width, height) / outputSize;
    uint2 last = (pixel + 1u) * uint2(width, height) / outputSize;
    float nearestDepth = 0.0;
    for (uint y = first.y; y < last.y; ++y)
    {
        for (uint x = first.x; x < last.x; ++x)
        {
            // Reversed-Z: keep foreground geometry instead of averaging it with empty background.
            nearestDepth = max(nearestDepth, aoFullDepth.Load(int3(x, y, 0)));
        }
    }
    return nearestDepth;
}

float PSUpsample(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float depth = aoFullDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        return 1.0;
    }
    uint width, height, fullWidth, fullHeight;
    aoDepth.GetDimensions(width, height);
    aoFullDepth.GetDimensions(fullWidth, fullHeight);
    float2 uv = position.xy / float2(fullWidth, fullHeight);
    float viewZ = reconstructViewPosition(uv, depth).z;
    float2 coordinate = uv * float2(width, height) - 0.5;
    int2 first = int2(floor(coordinate));
    float2 fraction = frac(coordinate);
    float total = 0.0;
    float weights = 0.0;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 neighbor = clamp(first + int2(x, y), int2(0, 0), int2(width, height) - 1);
            float neighborDepth = aoDepth.Load(int3(neighbor, 0));
            if (neighborDepth <= 0.0)
            {
                continue;
            }
            float neighborZ = reconstructViewPosition((float2(neighbor) + 0.5) / float2(width, height), neighborDepth).z;
            if (abs(viewZ - neighborZ) > aoFilter.z)
            {
                continue;
            }
            float2 weight = lerp(1.0 - fraction, fraction, float2(x, y));
            float combined = weight.x * weight.y;
            total += combined * aoInput.Load(int3(neighbor, 0));
            weights += combined;
        }
    }
    return weights > 1e-6 ? saturate(total / weights) : 1.0;
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
