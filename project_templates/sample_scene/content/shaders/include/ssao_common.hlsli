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

Texture2D<float2> aoNormals : register(t0); // Sphere-map view normals from z-prepass.
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

float3 readViewNormal(int2 pixel)
{
    // Matches the sphere-map encoding written by zprepassPSMain.
    float2 encoded = aoNormals.Load(int3(pixel, 0)).xy * 2.0 - 1.0;
    float lengthSquared = dot(encoded, encoded);
    return normalize(float3(2.0 * encoded * sqrt(max(0.0, 1.0 - lengthSquared)), 1.0 - 2.0 * lengthSquared));
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
