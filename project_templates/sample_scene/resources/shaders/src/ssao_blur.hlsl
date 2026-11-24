// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ssao_common.hlsli"
#include "ssao_reconstruction.hlsli"

Texture2D<float> aoFullDepth : register(t3);

struct DownsampleOutput
{
    float2 normal : SV_Target0;
    float depth : SV_Target1;
};

DownsampleOutput PSDownsample(float4 position : SV_Position)
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

float4 PSDebugBilinear(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    aoFullDepth.GetDimensions(width, height);
    float fullDepth = aoFullDepth.Load(int3(int2(position.xy), 0));
    float ao = ssaoReconstruct(position.xy / float2(width, height), fullDepth, aoInput, aoDepth,
                               aoLinearClamp, aoFilter.z, float2(aoProjection[2][3], aoProjection[2][2]));
    return float4(ao.xxx, 1.0);
}
