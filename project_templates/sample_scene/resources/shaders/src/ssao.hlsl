// Copyright 2026 N-GINN LLC. All rights reserved.
// Fourteen-sample SSAO following Bogdanov M.K.'s Shaders/Common/Ssao.hlsl.
#include "ssao_common.hlsli"
#include "hbao_common.hlsli"

static const float3 SAMPLE_DIRECTIONS[14] = {
    float3(1, 1, 1), float3(-1, -1, -1), float3(-1, 1, 1), float3(1, -1, -1),
    float3(1, 1, -1), float3(-1, -1, 1), float3(-1, 1, -1), float3(1, -1, 1),
    float3(-1, 0, 0), float3(1, 0, 0), float3(0, -1, 0), float3(0, 1, 0),
    float3(0, 0, -1), float3(0, 0, 1)
};

float PSNau(float4 position : SV_Position) : SV_Target
{
    float2 centerUv = position.xy / aoSizes.xy;
    int2 pixel = min(int2(centerUv * aoSizes.zw), int2(aoSizes.zw)-1);
    float depth = depthAtPixel(pixel);
    if (depth <= 0.0 || aoFilter.w == 0.0 || aoParameters.x <= 0.0)
    {
        return 1.0;
    }
    uint width, height;
    aoDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float3 center = reconstructViewPosition(centerUv, depth);
    float3 normal = readViewNormal(pixel);
    float3 random = randomDirection(uint2(pixel));
    float occlusion = 0.0;
    [unroll]
    for (uint index = 0; index < 14; ++index)
    {
        if (index >= uint(aoOptions.w)) break;
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
        float neighborDepth = depthAtPixel(neighbor);
        if (neighborDepth <= 0.0)
        {
            continue;
        }
        // Reconstruct at the actual sampled pixel center to avoid planar self-occlusion.
        float3 occluder = reconstructViewPosition((float2(neighbor) + 0.5) / size, neighborDepth);
        float3 delta = occluder - center;
        float distanceSquared = dot(delta, delta);
        // Reject the receiver's plane, including when it is tilted away from the eye.
        // A camera-Z test loses the farther face of a concave corner.
        float surfaceDistance = dot(normal, delta);
        if (surfaceDistance > aoParameters.w && distanceSquared > 1e-8)
        {
            float distance = sqrt(distanceSquared);
            float fade = saturate((aoParameters.z - distance) / max(aoParameters.z - aoParameters.y, 1e-5));
            occlusion += surfaceDistance / distance * fade;
        }
    }
    return pow(saturate(1.0 - occlusion / aoOptions.w), 6.0);
}

// Bogdanov's complete raw AO path: full-size inputs, point normal, bilinear depth,
// a 256x256 random-vector texture repeated four times, and view-depth attenuation.
// Adapted from Shaders/Common/Ssao.hlsl at d17e45bc for Nau's right-handed view space.
float PSBogdanov(float4 position : SV_Position) : SV_Target
{
    float2 uv = position.xy / aoFilter.xy;
    float depth = depthAtUv(uv);
    if (depth <= 0.0 || aoFilter.w == 0.0 || aoParameters.x <= 0.0)
    {
        return 1.0;
    }
    float3 center = reconstructViewPosition(uv, depth);
    float3 normal = readViewNormalAtUv(uv);
    float3 random = 2.0 * aoRandomMap.SampleLevel(aoLinearWrap, 4.0 * uv, 0).rgb - 1.0;
    float occlusion = 0.0;
    [unroll]
    for (uint index = 0; index < 14; ++index)
    {
        if (index >= uint(aoOptions.w)) break;
        float sampleLength = 0.25 + 0.75 * float(aoHash(index + 17u) & 65535u) / 65535.0;
        float3 offset = reflect(normalize(SAMPLE_DIRECTIONS[index]) * sampleLength, random);
        float3 samplePoint = center + sign(dot(offset, normal)) * aoParameters.x * offset;
        float4 projected = mul(aoProjection, float4(samplePoint, 1.0));
        if (projected.w <= 0.0 || abs(samplePoint.z) < 1e-7)
        {
            continue;
        }
        float2 sampleUv = projected.xy / projected.w * float2(0.5, -0.5) + 0.5;
        if (aoPipeline.z != 0.0 && (any(sampleUv < 0.0) || any(sampleUv >= 1.0))) continue;
        float neighborDepth = depthAtUv(sampleUv);
        if (neighborDepth <= 0.0)
        {
            continue;
        }
        float neighborZ = reconstructViewPosition(sampleUv, neighborDepth).z;
        float3 occluder = samplePoint * (neighborZ / samplePoint.z);
        float3 delta = occluder - center;
        float distanceSquared = dot(delta, delta);
        // The reference uses p.z-r.z with +Z forward. Nau has -Z forward.
        float distZ = occluder.z - center.z;
        if (distZ > aoParameters.w && distanceSquared > 1e-8)
        {
            float facing = max(dot(normal, delta * rsqrt(distanceSquared)), 0.0);
            float fade = saturate((aoParameters.z - distZ) / max(aoParameters.z - aoParameters.y, 1e-5));
            occlusion += facing * fade;
        }
    }
    return saturate(pow(1.0 - occlusion / aoOptions.w, 6.0));
}

float PSMain(float4 position : SV_Position) : SV_Target
{
    if (aoPipeline.w == 2.0) return computeHbao(uint2(position.xy));
    return aoPipeline.w != 0.0 ? PSBogdanov(position) : PSNau(position);
}
