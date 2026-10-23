// Copyright 2026 N-GINN LLC. All rights reserved.
// The same compiled shader is used by the engine on GPU 0 and the MG-10 pass on GPU 1.
cbuffer PassConstants : register(b0)
{
    float4 g_passConstants; // Integer animation phase, width, height, reserved.
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    uint phase = (uint)g_passConstants.x;
    uint2 pixel = (uint2)position.xy;
    bool odd = (((pixel.x + phase * 3) / 48) ^ (pixel.y / 48)) & 1;
    uint marker = phase * ((uint)g_passConstants.y - 1) / 255;
    float3 color = odd ? float3(phase, 180, 255 - phase) : float3(255 - phase, 40, phase);
    if (abs((int)pixel.x - (int)marker) < 8)
    {
        color = float3(255, 255, 255);
    }
    return float4(color / 255.0, 1.0);
}
