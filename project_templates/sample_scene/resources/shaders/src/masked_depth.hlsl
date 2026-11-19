// Copyright 2026 N-GINN LLC. All rights reserved.

#include "shader_defines.h"
#include "common_types.hlsli"
#include "in_out.hlsli"
#include "foliage_alpha.hlsli"

GLOBAL_CBUFFER(SceneBuffer) : register(b0)
{
    float4x4 vp;
    float4x4 prepassView;
    float4 instanceBaseID;
};

StructuredBuffer<InstanceData> instanceBuffer : register(t0);
Texture2D albedoTex : register(t0);
SamplerState albedoSampler : register(s0);

struct MaskedDepthOutput
{
    precise float4 position : SV_Position;
    nointerpolation uint4 uid : TEXCOORD0;
    float3 viewNormal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

MaskedDepthOutput VSMain(VsInput input, uint instID : SV_InstanceID)
{
    MaskedDepthOutput output;
    const uint instIdx = instanceBaseID.x + instID;
    output.position = mul(mul(vp, instanceBuffer[instIdx].worldMatrix), float4(input.position, 1.0f));
    output.uid = instanceBuffer[instIdx].uid;
    float3 worldNormal = mul((float3x3)instanceBuffer[instIdx].normalMatrix, input.normal);
    output.viewNormal = normalize(mul((float3x3)prepassView, worldNormal));
    output.texCoord = float2(input.texCoord.x, 1.0f - input.texCoord.y);
    return output;
}

uint4 PSDepth(MaskedDepthOutput input) : SV_Target0
{
    clipFoliageAlpha(albedoTex.Sample(albedoSampler, input.texCoord).a);
    return input.uid;
}

struct MaskedNormalsOutput
{
    uint4 uid : SV_Target0;
    float2 normal : SV_Target1;
};

MaskedNormalsOutput PSNormals(MaskedDepthOutput input, bool frontFace : SV_IsFrontFace)
{
    clipFoliageAlpha(albedoTex.Sample(albedoSampler, input.texCoord).a);
    MaskedNormalsOutput output;
    output.uid = input.uid;
    float3 normal = normalize(input.viewNormal) * (frontFace ? 1.0f : -1.0f);
    float lengthSquared = dot(normal.xy, normal.xy);
    float2 direction = lengthSquared > 0.0f ? normal.xy * rsqrt(lengthSquared) : float2(1.0f, 0.0f);
    output.normal = direction * sqrt(saturate(0.5f - normal.z * 0.5f)) * 0.5f + 0.5f;
    return output;
}
