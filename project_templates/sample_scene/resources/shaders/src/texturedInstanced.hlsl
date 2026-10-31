// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "common_types.hlsli"
#include "in_out.hlsli"
#include "shader_defines.h"


GLOBAL_CBUFFER(SceneBuffer) : register(b0)
{
    float4x4 vp;
    float4x4 mvp;
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4x4 prepassView;
    float4 instanceBaseID;
    matrix BonesTransforms[NAU_MAX_SKINNING_BONES_COUNT];
    matrix BonesNormalTransforms[NAU_MAX_SKINNING_BONES_COUNT];
};

cbuffer ColorBuffer : register(b1)
{
    float4 color;
};

StructuredBuffer<InstanceData> instanceBuffer : register(t0);

Texture2D tex : register(t0);
SamplerState sampl : register(s0);

float4 PSMain(VsOutput input) : SV_Target
{
    float4 albedo = tex.Sample(sampl, input.texCoord);
    return albedo * input.color;
}

VsOutputZPrepass zprepassInstancedVSMain(VsInputZPrepass input, uint instID : SV_InstanceID)
{
    VsOutputZPrepass output;

    const uint instIdx = instanceBaseID.x + instID;
    output.uid = instanceBuffer[instIdx].uid;
    output.position = mul(mul(vp, instanceBuffer[instIdx].worldMatrix), float4(input.position, 1.0f));
    float3 worldNormal = mul((float3x3)instanceBuffer[instIdx].normalMatrix, input.normal);
    output.viewNormal = normalize(mul((float3x3)prepassView, worldNormal));

    return output;
}

VsOutputZPrepass zprepassSkinnedVSMain(VsInputZPrepassSkinned input, uint instID : SV_InstanceID)
{
    VsOutputZPrepass output;

    const uint instIdx = instanceBaseID.x + instID;
    output.uid = instanceBuffer[instIdx].uid;

    float4 skinnedPos = float4(0.0, 0.0, 0.0, 0.0);
    float3 skinnedNormal = float3(0.0, 0.0, 0.0);

    for (int i = 0; i < 4; i++)
    {
        const uint boneIndex = input.boneIndices[i];
        if (boneIndex < NAU_MAX_SKINNING_BONES_COUNT)
        {
            skinnedPos += input.boneWeights[i] * mul(BonesTransforms[boneIndex], float4(input.position, 1.0));
            skinnedNormal += input.boneWeights[i] * mul((float3x3)BonesNormalTransforms[boneIndex], input.normal);
        }
    }

    output.position = mul(vp, skinnedPos);
    output.viewNormal = normalize(mul((float3x3)prepassView, skinnedNormal));

    return output;
}

struct PsOutputZPrepass
{
    uint4 uid : SV_Target0;
    float2 normal : SV_Target1;
};

uint4 zprepassPSMain(VsOutputZPrepass input) : SV_Target0
{
    return input.uid;
}

PsOutputZPrepass zprepassNormalsPSMain(VsOutputZPrepass input)
{
    PsOutputZPrepass output;
    output.uid = input.uid;
    float3 normal = normalize(input.viewNormal);
    float lengthSquared = dot(normal.xy, normal.xy);
    // Both poles must stay finite, including the back-facing (0, 0, -1) normal.
    float2 direction = lengthSquared > 0.0 ? normal.xy * rsqrt(lengthSquared) : float2(1.0, 0.0);
    output.normal = direction * sqrt(saturate(0.5 - normal.z * 0.5)) * 0.5 + 0.5;
    return output;
}

VsOutputZPrepass outlineMaskVSMain(VsInputZPrepass input, uint instID : SV_InstanceID)
{
    VsOutputZPrepass output = (VsOutputZPrepass)0;

    const uint instIdx = instanceBaseID.x + instID;
    output.uid.x = instanceBuffer[instIdx].isHighlighted;
    output.position = mul(mul(vp, instanceBuffer[instIdx].worldMatrix), float4(input.position, 1.0f));

    return output;
}

VsOutputZPrepass outlineMaskSkinnedVSMain(VsInputZPrepassSkinned input, uint instID : SV_InstanceID)
{
    VsOutputZPrepass output = (VsOutputZPrepass)0;

    const uint instIdx = instanceBaseID.x + instID;
    output.uid.x = instanceBuffer[instIdx].isHighlighted;

    float4 skinnedPos = float4(0.0, 0.0, 0.0, 0.0);

    for (int i = 0; i < 4; i++)
    {
        const uint boneIndex = input.boneIndices[i];
        if (boneIndex < NAU_MAX_SKINNING_BONES_COUNT)
        {
            skinnedPos += input.boneWeights[i] * mul(BonesTransforms[boneIndex], float4(input.position, 1.0));
        }
    }

    output.position = mul(vp, mul(worldMatrix, skinnedPos));

    return output;
}

float outlineMaskPSMain(VsOutputZPrepass input) : SV_Target
{
    return input.uid.x;
}
