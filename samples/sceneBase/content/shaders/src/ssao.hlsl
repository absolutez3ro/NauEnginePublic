// Copyright 2026 N-GINN LLC. All rights reserved.
// Fourteen-sample SSAO following Bogdanov M.K.'s Shaders/Common/Ssao.hlsl.
#include "ssao_common.hlsli"

static const float3 SAMPLE_DIRECTIONS[14] = {
    float3(1, 1, 1), float3(-1, -1, -1), float3(-1, 1, 1), float3(1, -1, -1),
    float3(1, 1, -1), float3(-1, -1, 1), float3(-1, 1, -1), float3(1, -1, 1),
    float3(-1, 0, 0), float3(1, 0, 0), float3(0, -1, 0), float3(0, 1, 0),
    float3(0, 0, -1), float3(0, 0, 1)
};

float PSMain(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float depth = aoDepth.Load(int3(pixel, 0));
    if (depth <= 0.0 || aoFilter.w == 0.0 || aoParameters.x <= 0.0)
    {
        return 1.0;
    }
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float3 center = reconstructViewPosition(position.xy / size, depth);
    float3 normal = readViewNormal(pixel);
    float3 random = randomDirection(uint2(pixel));
    float occlusion = 0.0;
    [unroll]
    for (uint index = 0; index < 14; ++index)
    {
        float sampleLength = 0.25 + 0.75 * float(aoHash(index + 17u) & 65535u) / 65535.0;
        float3 offset = reflect(normalize(SAMPLE_DIRECTIONS[index]), random) * sampleLength;
        offset *= dot(offset, normal) >= 0.0 ? 1.0 : -1.0;
        float3 samplePoint = center + aoParameters.x * offset;
        float4 projected = mul(aoProjection, float4(samplePoint, 1.0));
        // Reject samples behind the eye, before the near plane, or outside the image.
        if (projected.w <= 0.0 || projected.z < 0.0 || projected.z > projected.w)
        {
            continue;
        }
        float2 uv = projected.xy / projected.w * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv >= 1.0))
        {
            continue;
        }
        int2 neighbor = int2(uv * size);
        float neighborDepth = aoDepth.Load(int3(neighbor, 0));
        if (neighborDepth <= 0.0)
        {
            continue;
        }
        // Reconstruct at the actual sampled pixel center to avoid planar self-occlusion.
        float3 occluder = reconstructViewPosition((float2(neighbor) + 0.5) / size, neighborDepth);
        float3 delta = occluder - center;
        float distanceSquared = dot(delta, delta);
        // Nau looks along -Z; a point nearer the eye has a larger view-space Z.
        float distanceZ = occluder.z - center.z;
        if (distanceZ > aoParameters.w && distanceSquared > 1e-8)
        {
            float fade = saturate((aoParameters.z - distanceZ) / max(aoParameters.z - aoParameters.y, 1e-5));
            occlusion += max(dot(normal, delta * rsqrt(distanceSquared)), 0.0) * fade;
        }
    }
    return pow(saturate(1.0 - occlusion / 14.0), 6.0);
}
