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
    float4 aoFilter;     // Depth threshold in z.
    float4 aoOptions;
    float4 aoPipeline;   // AO algorithm in w.
    float4 aoSizes;      // AO width/height, input width/height.
    float4 aoReconstruction;
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

float depthAtPixel(int2 pixel)
{
    return aoDepth.Load(int3(clamp(pixel, int2(0, 0), int2(aoSizes.zw) - 1), 0));
}

float3 reconstructNormal(int2 pixel)
{
    int2 size = int2(aoSizes.zw);
    int2 c = clamp(pixel, int2(0, 0), size - 1);
    int2 l = max(c - int2(1,0), 0), r = min(c + int2(1,0), size - 1);
    int2 t = max(c - int2(0,1), 0), b = min(c + int2(0,1), size - 1);
    float3 pc = reconstructViewPosition((c + 0.5) / size, depthAtPixel(c));
    float3 pl = reconstructViewPosition((l + 0.5) / size, depthAtPixel(l));
    float3 pr = reconstructViewPosition((r + 0.5) / size, depthAtPixel(r));
    float3 pt = reconstructViewPosition((t + 0.5) / size, depthAtPixel(t));
    float3 pb = reconstructViewPosition((b + 0.5) / size, depthAtPixel(b));
    float3 dx = c.x == 0 ? pr - pc : (c.x == size.x - 1 || abs(pl.z-pc.z) < abs(pr.z-pc.z) ? pc-pl : pr-pc);
    float3 dy = c.y == 0 ? pb - pc : (c.y == size.y - 1 || abs(pt.z-pc.z) < abs(pb.z-pc.z) ? pc-pt : pb-pc);
    float3 n = cross(dy, dx);
    n = dot(n,n) > 1e-12 ? normalize(n) : float3(0,0,1);
    return dot(n, -pc) >= 0.0 ? n : -n;
}

uint aoHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

// Fixed 4x4 tile for rotating SSAO samples.
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
