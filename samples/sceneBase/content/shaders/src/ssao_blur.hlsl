// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ssao_common.hlsli"
#include "ssao_reconstruction.hlsli"

Texture2D<float> aoFullDepth : register(t3);

float PSDownsample(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoDepth.GetDimensions(width, height);
    const int2 stride = int2(ceil(float2(width, height) / aoSizes.zw));
    const int2 pixel = min(int2(position.xy) * stride, int2(width, height) - 1);
    return aoDepth.Load(int3(pixel, 0));
}

struct DownsampleOutput
{
    float2 normal : SV_Target0;
    float depth : SV_Target1;
};

DownsampleOutput PSDownsampleNormals(float4 position : SV_Position)
{
    uint width, height;
    aoDepth.GetDimensions(width, height);
    int2 stride = int2(ceil(float2(width, height) / aoSizes.zw));
    int2 pixel = min(int2(position.xy) * stride, int2(width, height) - 1);
    DownsampleOutput result;
    result.normal = aoNormals.Load(int3(pixel, 0));
    result.depth = aoDepth.Load(int3(pixel, 0));
    return result;
}

float PSBoxBlur(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float2 projection = float2(aoProjection[2][3], aoProjection[2][2]);
    float centerDepth = ssaoLinearDepth(aoDepth.Load(int3(pixel, 0)), projection);
    float total = 0.0;
    float weightSum = 0.0;

    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            float2 uv = (pixel + 2 * int2(x, y)) / aoSizes.xy;
            float4 depths = aoDepth.GatherRed(aoLinearClamp, uv);
            float4 values = aoInput.GatherRed(aoLinearClamp, uv);
            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                float distance = abs(ssaoLinearDepth(depths[i], projection) - centerDepth);
                float weight = saturate(1.0 - distance / aoFilter.z);
                total += values[i] * weight;
                weightSum += weight;
            }
        }
    }
    return weightSum > 1e-6 ? saturate(total / weightSum) : aoInput.Load(int3(pixel, 0));
}

float4 PSDebugBilinear(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoFullDepth.GetDimensions(width, height);
    float fullDepth = aoFullDepth.Load(int3(int2(position.xy), 0));
    float ao = ssaoReconstruct(position.xy / float2(width, height), fullDepth, aoInput, aoDepth,
                               aoLinearClamp, aoFilter.z, float2(aoProjection[2][3], aoProjection[2][2]));
    return float4(ao.xxx, 1.0);
}
