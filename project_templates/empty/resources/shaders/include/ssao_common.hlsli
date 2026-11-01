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
Texture2D<float4> aoRandomMap : register(t4);
SamplerState aoLinearClamp : register(s0);
SamplerState aoLinearWrap : register(s1);

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
    // Matches the sphere-map encoding written by zprepassNormalsPSMain.
    float2 encoded = aoNormals.Load(int3(pixel, 0)).xy * 2.0 - 1.0;
    float lengthSquared = dot(encoded, encoded);
    return normalize(float3(2.0 * encoded * sqrt(max(0.0, 1.0 - lengthSquared)), 1.0 - 2.0 * lengthSquared));
}

float3 readViewNormalAtUv(float2 uv)
{
    uint width, height;
    aoNormals.GetDimensions(width, height);
    return readViewNormal(clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width, height) - 1));
}

uint aoHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

// A fixed 4x4 tile, covered completely by the GPU 0 box filter.
static const float3 RANDOM_DIRECTIONS[16] = {
    float3(0.11723767, -0.66131229, 0.74089229),
    float3(-0.11133004, 0.91610514, -0.38517138),
    float3(-0.60318909, 0.59611841, -0.52991109),
    float3(-0.63597903, 0.47813760, -0.60573518),
    float3(0.42028056, 0.86470351, -0.27504923),
    float3(-0.70626777, -0.29203203, 0.64490552),
    float3(0.19309230, 0.79014831, -0.58170526),
    float3(0.52244947, -0.66537250, -0.53322226),
    float3(-0.16402077, -0.32514631, -0.93133080),
    float3(0.46191547, -0.45082770, -0.76379872),
    float3(-0.43681689, -0.25866798, -0.86155782),
    float3(-0.87224651, -0.43429847, -0.22487968),
    float3(-0.38795436, 0.64077083, -0.66249842),
    float3(-0.94566205, 0.25598346, -0.20048879),
    float3(-0.55571995, 0.62633098, -0.54670362),
    float3(-0.96326560, -0.15818593, -0.21701751)
};

float3 randomDirection(uint2 pixel)
{
    return RANDOM_DIRECTIONS[(pixel.x & 3u) + (pixel.y & 3u) * 4u];
}
#endif
