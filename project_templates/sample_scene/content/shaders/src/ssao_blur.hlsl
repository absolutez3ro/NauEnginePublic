// Copyright 2026 N-GINN LLC. All rights reserved.
// Separable bilateral Gaussian blur following Bogdanov M.K.'s SsaoBlur.hlsl.
#include "ssao_common.hlsli"

cbuffer AmbientOcclusionReprojection : register(b1)
{
    float4x4 aoCurrentToHistory;
    float4x4 aoHistoryInverseProjection;
    float4 aoReprojectionParameters; // Absolute view-space depth tolerance.
};

Texture2D<float> aoHistoryDepth : register(t3);

float PSReproject(float4 position : SV_Position) : SV_Target
{
    float depth = aoDepth.Load(int3(int2(position.xy), 0));
    if (depth <= 0.0)
    {
        return 1.0;
    }
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 currentNdc = position.xy / size * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 historyClip = mul(aoCurrentToHistory, float4(currentNdc, depth, 1.0));
    if (!all(isfinite(historyClip)) || historyClip.w <= 0.0 || historyClip.z < 0.0 || historyClip.z > historyClip.w)
    {
        return 1.0;
    }
    float3 historyNdc = historyClip.xyz / historyClip.w;
    float2 historyUv = historyNdc.xy * float2(0.5, -0.5) + 0.5;
    if (any(historyUv < 0.0) || any(historyUv >= 1.0))
    {
        return 1.0;
    }
    float4 expectedView = mul(aoHistoryInverseProjection, float4(historyNdc, 1.0));
    if (abs(expectedView.w) <= 1e-8)
    {
        return 1.0;
    }
    float expectedZ = expectedView.z / expectedView.w;
    float tolerance = max(aoReprojectionParameters.x, abs(expectedZ) * 0.001);
    float2 historyPixel = historyUv * size - 0.5;
    int2 origin = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    float visibility = 1.0;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 pixel = origin + int2(x, y);
            if (any(pixel < 0) || any(pixel >= int2(width, height)))
            {
                continue;
            }
            float previousDepth = aoHistoryDepth.Load(int3(pixel, 0));
            if (previousDepth <= 0.0)
            {
                continue;
            }
            float2 tapNdc = (float2(pixel) + 0.5) / size * float2(2.0, -2.0) + float2(-1.0, 1.0);
            float4 previousView = mul(aoHistoryInverseProjection, float4(tapNdc, previousDepth, 1.0));
            if (abs(previousView.w) <= 1e-8 || abs(previousView.z / previousView.w - expectedZ) > tolerance)
            {
                continue;
            }
            float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
            // Invalid taps remain unoccluded instead of bleeding another surface's AO across an edge.
            visibility += weight * (aoInput.Load(int3(pixel, 0)) - 1.0);
        }
    }
    return saturate(visibility);
}

float2 PSPackNormals(float4 position : SV_Position) : SV_Target
{
    return aoNormals.Load(int3(int2(position.xy), 0)).xy;
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
