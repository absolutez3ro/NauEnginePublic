// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#ifndef OMNI_SHADOW_HLSLI
#define OMNI_SHADOW_HLSLI 1

#define OMNI_SHADOW_MAX_SLOTS 2
#define OMNI_SHADOW_FACES 6
#define OMNI_SHADOW_ATLAS_SIZE float2(1536.0, 2048.0)

struct OmniShadowSlot
{
    float4x4 faceViewProj[OMNI_SHADOW_FACES];
    float4 faceRect[OMNI_SHADOW_FACES];
    float4 posRadius;
    float4 params;
};

SYSTEM_CBUFFER(OmniShadowBuffer) :
    register(b3)
{
    float4 omni_shadow_header;
    OmniShadowSlot omni_shadow_slots[OMNI_SHADOW_MAX_SLOTS];
}

Texture2D omni_shadow_tex : register(t8);
SamplerComparisonState omni_shadow_smp : register(s8);

uint omniShadowFace(float3 d)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
        return d.x >= 0 ? 0 : 1;
    if (a.y >= a.z)
        return d.y >= 0 ? 2 : 3;
    return d.z >= 0 ? 4 : 5;
}

float getOmniShadow(float4 shadowTcToAtlas, float4 pos_and_radius, float3 worldPos, float NoL, float2 screenpos)
{
    if (shadowTcToAtlas.x < -0.5)
        return 1.0;

    const int slot = (int)round(shadowTcToAtlas.x);
    if (slot < 0 || slot >= (int)omni_shadow_header.x)
        return 1.0;

    const OmniShadowSlot s = omni_shadow_slots[slot];
    const uint face = omniShadowFace(worldPos - s.posRadius.xyz);

    const float4 clip = mul(s.faceViewProj[face], float4(worldPos, 1.0));
    if (clip.w <= 0.0)
        return 1.0;

    const float n = s.params.z;
    const float f = s.params.w;
    const float texelWorld = 2.0 * clip.w / 512.0;
    const float cosA = saturate(NoL);
    const float tanA = min(sqrt(max(1.0 - cosA * cosA, 0.0)) / max(cosA, 0.05), 3.0);
    const float biasMeters = s.params.x + 0.2 * texelWorld * (1.0 + tanA);
    const float d = max(clip.w - biasMeters, n);
    const float reference = f * (d - n) / (d * (f - n));

    const float4 rect = s.faceRect[face];
    const float2 texel = 1.0 / OMNI_SHADOW_ATLAS_SIZE;
    const float2 halfSize = abs(rect.xy);
    const float2 uv = clamp(clip.xy / clip.w * rect.xy + rect.zw, rect.zw - halfSize + texel * 1.5, rect.zw + halfSize - texel * 1.5);

    float lit = 0.0;
    lit += omni_shadow_tex.SampleCmpLevelZero(omni_shadow_smp, uv + texel * float2(-0.5, -0.5), reference);
    lit += omni_shadow_tex.SampleCmpLevelZero(omni_shadow_smp, uv + texel * float2(0.5, -0.5), reference);
    lit += omni_shadow_tex.SampleCmpLevelZero(omni_shadow_smp, uv + texel * float2(-0.5, 0.5), reference);
    lit += omni_shadow_tex.SampleCmpLevelZero(omni_shadow_smp, uv + texel * float2(0.5, 0.5), reference);
    return lit * 0.25;
}

#endif
