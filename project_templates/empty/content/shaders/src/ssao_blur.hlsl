// Copyright 2026 N-GINN LLC. All rights reserved.
// Full-resolution bilateral blur and half-resolution secondary AO postprocessing.
#include "ssao_common.hlsli"

cbuffer AmbientOcclusionHalfConstants : register(b1)
{
    float4 aoDepthParameters; // Projection B, C, A, depth threshold.
    float4 aoHalfControl;     // Blur enabled.
};

Texture2D<float> aoFullDepth : register(t3);

struct DownsampleOutput
{
    float2 normal : SV_Target0;
    float depth : SV_Target1;
};

DownsampleOutput PSDownsample(float4 position : SV_Position)
{
    // The same real surface sample supplies both depth and normal, including odd-sized edges.
    uint width, height;
    aoDepth.GetDimensions(width, height);
    int2 pixel = min(int2(position.xy) * 2, int2(width, height) - 1);
    DownsampleOutput output;
    output.normal = aoNormals.Load(int3(pixel, 0));
    output.depth = aoDepth.Load(int3(pixel, 0));
    return output;
}

float linearViewDepth(float depth)
{
    // Perspective z from the projection coefficients: one division, no matrix multiplication.
    return aoDepthParameters.x / (max(depth, 1e-7) * aoDepthParameters.y - aoDepthParameters.z);
}

float PSHalfBlur(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (aoHalfControl.x == 0.0)
    {
        return aoInput.Load(int3(pixel, 0));
    }
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float centerZ = linearViewDepth(aoDepth.Load(int3(pixel, 0)));
    float total = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            int2 neighbor = clamp(pixel + int2(x, y), int2(0, 0), int2(width, height) - 1);
            float neighborZ = linearViewDepth(aoDepth.Load(int3(neighbor, 0)));
            float weight = saturate(1.0 - abs(neighborZ - centerZ) / max(aoDepthParameters.w, 1e-5));
            total += weight * aoInput.Load(int3(neighbor, 0));
            weightSum += weight;
        }
    }
    return saturate(total / weightSum);
}

float PSUpsample(float4 position : SV_Position) : SV_Target0
{
    uint width, height;
    aoDepth.GetDimensions(width, height);
    int2 origin = int2(floor(position.xy * 0.5 - 0.5));
    float fullZ = linearViewDepth(aoFullDepth.Load(int3(int2(position.xy), 0)));
    float nearestDistance = 3.402823e+38;
    int2 nearest = clamp(origin, int2(0, 0), int2(width, height) - 1);
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 neighbor = clamp(origin + int2(x, y), int2(0, 0), int2(width, height) - 1);
            float distance = abs(linearViewDepth(aoDepth.Load(int3(neighbor, 0))) - fullZ);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = neighbor;
            }
        }
    }
    // Always use the nearest surface's AO; a white fallback opens holes around thin geometry.
    return aoInput.Load(int3(nearest, 0));
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
