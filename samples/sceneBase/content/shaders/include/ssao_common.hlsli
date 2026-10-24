// Copyright 2026 N-GINN LLC. All rights reserved.
// Adapted from Bogdanov M.K.'s Multi-GPU-Render-DX12, commit 9146f83f.
#ifndef NAU_SSAO_COMMON
#define NAU_SSAO_COMMON

cbuffer AmbientOcclusionConstants : register(b0)
{
    float4x4 aoProjection;
    float4x4 aoInverseProjection;
    float4x4 aoView;
    float4 aoParameters; // Radius, fade start, fade end, surface epsilon, in scene units.
    float4 aoFilter;     // Horizontal pixel step, vertical pixel step, depth threshold, enabled.
};

Texture2D<float4> aoNormals : register(t0); // Sphere-map world normals in .xy.
Texture2D<float> aoDepth : register(t1);    // Reversed-Z: background = 0.
Texture2D<float> aoInput : register(t2);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 reconstructViewPosition(float2 uv, float depth)
{
    float4 position = mul(aoInverseProjection, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
    return position.xyz / position.w;
}

precise float4 readDepthDerivative(int2 pixel, int2 offset, int2 size, float depth, float4 center)
{
    int2 neighbor = pixel + offset;
    if (any(neighbor < 0) || any(neighbor >= size))
    {
        return 0.0;
    }
    float neighborDepth = aoDepth.Load(int3(neighbor, 0));
    if (neighborDepth <= 0.0)
    {
        return 0.0;
    }
    float2 ndcOffset = float2(offset) * float2(2.0, -2.0) / float2(size);
    precise float4 delta = mul(aoInverseProjection, float4(ndcOffset, neighborDepth - depth, 0.0));
    // P(neighbor)-P(center) is proportional to this expression. Avoid subtracting two large view positions.
    return float4(delta.xyz - center.xyz * (delta.w / center.w), abs(center.w + delta.w));
}

precise float3 depthDerivative(float4 negative, float4 positive)
{
    if (negative.w == 0.0)
    {
        return positive.xyz;
    }
    return positive.w == 0.0 || abs(negative.z * positive.w) < abs(positive.z * negative.w) ? negative.xyz : positive.xyz;
}

precise float3 reconstructDepthNormal(int2 pixel, int2 size)
{
    float depth = aoDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        return float3(0, 0, 1);
    }
    float2 uv = (float2(pixel) + 0.5) / float2(size);
    precise float4 center = mul(aoInverseProjection, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
    float4 left = readDepthDerivative(pixel, int2(-1, 0), size, depth, center);
    float4 right = readDepthDerivative(pixel, int2(1, 0), size, depth, center);
    float4 up = readDepthDerivative(pixel, int2(0, -1), size, depth, center);
    float4 down = readDepthDerivative(pixel, int2(0, 1), size, depth, center);
    left.xyz = -left.xyz;
    down.xyz = -down.xyz;
    // Prefer a neighbor on the same surface instead of spanning a silhouette/depth discontinuity.
    precise float3 dx = depthDerivative(left, right);
    precise float3 dy = depthDerivative(down, up);
    precise float3 normal = cross(dx, dy);
    float lengthSquared = dot(normal, normal);
    if (lengthSquared <= 1e-20)
    {
        return float3(0, 0, 1);
    }
    normal *= rsqrt(lengthSquared);
    return dot(normal, center.xyz) * center.w > 0.0 ? -normal : normal;
}

float3 readViewNormal(int2 pixel)
{
    // Matches sphere-map decoding in gbuffer_base.hlsli.
    float2 encoded = aoNormals.Load(int3(pixel, 0)).xy * 2.0 - 1.0;
    float lengthSquared = dot(encoded, encoded);
    float3 worldNormal = float3(2.0 * encoded * sqrt(max(0.0, 1.0 - lengthSquared)), 1.0 - 2.0 * lengthSquared);
    return normalize(mul((float3x3)aoView, worldNormal));
}

uint aoHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float3 randomDirection(uint2 pixel)
{
    // A fixed seed keeps the pattern stable between frames and across devices.
    uint seed = aoHash(pixel.x + pixel.y * 65537u);
    float3 random = float3(aoHash(seed + 1u) & 65535u, aoHash(seed + 2u) & 65535u, aoHash(seed + 3u) & 65535u);
    return normalize(random / 32767.5 - 1.0);
}
#endif
