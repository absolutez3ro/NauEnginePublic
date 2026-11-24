// Copyright 2026 N-GINN LLC. All rights reserved.
// Native MGPU transport only. This preserves the original R16F/R32F values; it does not filter depth.
cbuffer AmbientOcclusionConstants : register(b0)
{
    float4x4 aoProjection;
    float4x4 aoInverseProjection;
    float4x4 aoView;
    float4 aoParameters;
    float4 aoFilter;
    float4 aoOptions;
};
Texture2D<uint4> packedDepth : register(t3);
Texture2D<uint4> packedNormals : register(t0);

float4 VSUnpackDepth(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float unpackDepth(uint2 pixel)
{
    float value = 0.0;
    if (aoOptions.z != 0.0)
    {
        uint4 words = packedDepth.Load(int3(pixel.x / 8, pixel.y, 0));
        uint bits = words[(pixel.x / 2) & 3];
        value = f16tof32((bits >> ((pixel.x & 1) * 16)) & 0xffff);
    }
    else
    {
        uint4 words = packedDepth.Load(int3(pixel.x / 4, pixel.y, 0));
        value = asfloat(words[pixel.x & 3]);
    }
    return value;
}

float PSUnpackDepth(float4 position : SV_Position) : SV_Target
{
    return unpackDepth(uint2(position.xy));
}

struct UnpackOutput { float depth : SV_Target0; float2 normal : SV_Target1; };
UnpackOutput PSUnpackDepthNormals(float4 position : SV_Position)
{
    uint2 pixel = uint2(position.xy);
    UnpackOutput result;
    result.depth = unpackDepth(pixel);
    if (aoOptions.y != 0.0)
    {
        uint4 words = packedNormals.Load(int3(pixel.x / 8, pixel.y, 0));
        uint bits = words[(pixel.x / 2) & 3] >> ((pixel.x & 1) * 16);
        result.normal = float2(bits & 255, (bits >> 8) & 255) / 255.0;
    }
    else
    {
        uint4 words = packedNormals.Load(int3(pixel.x / 4, pixel.y, 0));
        uint bits = words[pixel.x & 3];
        result.normal = float2(f16tof32(bits & 0xffff), f16tof32(bits >> 16));
    }
    return result;
}
