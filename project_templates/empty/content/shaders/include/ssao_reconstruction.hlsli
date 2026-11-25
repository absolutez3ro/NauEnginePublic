// Copyright 2026 N-GINN LLC. All rights reserved.
#ifndef NAU_SSAO_RECONSTRUCTION
#define NAU_SSAO_RECONSTRUCTION

float ssaoLinearDepth(float depth, float2 projection)
{
    return depth <= 0.0 ? 65504.0 : abs(projection.x / (-depth - projection.y));
}

float ssaoReconstruct(float2 uv, float fullDepth, Texture2D<float> ao, Texture2D<float> depth,
                      SamplerState linearClamp, float depthThreshold, float2 projection)
{
    uint width, height;
    ao.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 samplePosition = uv * size - 0.5;
    float2 origin = floor(samplePosition);
    float2 gatherUv = (origin + 1.0) / size;
    float4 values = ao.GatherRed(linearClamp, gatherUv).wzxy;
    float4 depths = depth.GatherRed(linearClamp, gatherUv).wzxy;
    float fullZ = ssaoLinearDepth(fullDepth, projection);
    float nearestDistance = 3.402823e+38;
    float nearestAo = 1.0;
    float weightedAo = 0.0;
    float weightSum = 0.0;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        float distance = abs(ssaoLinearDepth(depths[i], projection) - fullZ);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestAo = values[i];
        }

        float2 spatial = max(1.0 - abs(samplePosition - (origin + float2(i % 2, i / 2))), 0.0);
        float weight = spatial.x * spatial.y * saturate(1.0 - distance / max(depthThreshold, 1e-5));
        weightedAo += weight * values[i];
        weightSum += weight;
    }

    return weightSum <= 1e-6 ? nearestAo : saturate(weightedAo / weightSum);
}

#endif
