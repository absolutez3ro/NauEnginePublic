// Copyright 2026 N-GINN LLC. All rights reserved.
// Horizon search adapted from Bogdanov M.K., Shaders/Common/HBAO.hlsl, d17e45bc.
// Shared by the primary pixel path and the secondary compute path.
#ifndef NAU_HBAO_COMMON
#define NAU_HBAO_COMMON

#include "ssao_common.hlsli"

static const float HBAO_PI = 3.14159265359;
static const float HBAO_TAN_BIAS = 0.57735026919; // tan(30 degrees).
static const float HBAO_MAX_RADIUS_PIXELS = 64.0;
static const float HBAO_DISCARD_DISTANCE = 300.0;
static const float HBAO_INTENSITY = 1.9;
static const float HBAO_EPSILON = 1e-6;
static const int HBAO_DIRECTIONS = 16;
static const int HBAO_STEPS = 4;

float hbaoReadDepth(float2 uv)
{
    // The reference uses bilinear sampling with far depth at the border.
    // In reversed-Z this is zero, so fade the clamped sample by the in-bounds weight.
    float2 texel = uv * aoSizes.zw - 0.5;
    if (aoOptions.z == 0.0)
    {
        float2 coverage = saturate(min(texel + 1.0, aoSizes.zw - texel));
        return aoDepth.SampleLevel(aoLinearClamp, uv, 0) * coverage.x * coverage.y;
    }

    // Convert each R16F texel before filtering to reproduce interpolation in
    // projected depth. Texture2D.Load returns zero for out-of-bounds taps.
    int2 pixel = int2(floor(texel));
    float2 weight = frac(texel);
    float top = lerp(rawDepth(aoDepth.Load(int3(pixel, 0))),
                     rawDepth(aoDepth.Load(int3(pixel + int2(1, 0), 0))), weight.x);
    float bottom = lerp(rawDepth(aoDepth.Load(int3(pixel + int2(0, 1), 0))),
                        rawDepth(aoDepth.Load(int3(pixel + int2(1, 1), 0))), weight.x);
    return lerp(top, bottom, weight.y);
}

bool hbaoReadPosition(float2 uv, out float3 position)
{
    position = 0.0;
    float depth = hbaoReadDepth(uv);
    float4 p = mul(aoInverseProjection, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
    if (abs(p.w) <= HBAO_EPSILON) return false;
    // Reflect Nau's view Z so the original positive-Z tangent formula applies.
    position = p.xyz / p.w * float3(1.0, 1.0, -1.0);
    return all(isfinite(position)) && position.z > HBAO_EPSILON;
}

float3 hbaoPositionDifference(float2 uv, float2 offset, float3 center)
{
    float3 before, after;
    bool hasBefore = hbaoReadPosition(uv - offset, before);
    bool hasAfter = hbaoReadPosition(uv + offset, after);
    float3 negative = center - before;
    float3 positive = after - center;
    if (hasBefore && hasAfter)
        return dot(positive, positive) < dot(negative, negative) ? positive : negative;
    if (hasAfter) return positive;
    if (hasBefore) return negative;
    return 0.0;
}

float hbaoTangent(float3 delta)
{
    return delta.z * rsqrt(max(dot(delta.xy, delta.xy), HBAO_EPSILON * HBAO_EPSILON));
}

float hbaoTangentToSine(float tangent)
{
    return tangent * rsqrt(1.0 + tangent * tangent);
}

float2 hbaoSnapUvOffset(float2 offset, float2 inverseSize)
{
    return round(offset * aoSizes.xy) * inverseSize;
}

float hbaoHorizon(float2 uv, float3 center, float3 dx, float3 dy,
                  float2 deltaUv, float2 inverseSize, int steps, float jitter, float radiusSquared)
{
    uv += hbaoSnapUvOffset(jitter * deltaUv, inverseSize);
    deltaUv = hbaoSnapUvOffset(deltaUv, inverseSize);
    float3 tangent = deltaUv.x * dx + deltaUv.y * dy;
    float tangentHorizon = hbaoTangent(tangent) + HBAO_TAN_BIAS;
    float sineHorizon = hbaoTangentToSine(tangentHorizon);
    float occlusion = 0.0;
    for (int step = 0; step < steps; ++step)
    {
        uv += deltaUv;
        float3 samplePosition;
        if (!hbaoReadPosition(uv, samplePosition)) continue;
        float3 delta = samplePosition - center;
        float distanceSquared = dot(delta, delta);
        float sampleTangent = hbaoTangent(delta);
        if (distanceSquared < radiusSquared && sampleTangent > tangentHorizon)
        {
            float sampleSine = hbaoTangentToSine(sampleTangent);
            occlusion += saturate(1.0 - distanceSquared / radiusSquared) * (sampleSine - sineHorizon);
            tangentHorizon = sampleTangent;
            sineHorizon = sampleSine;
        }
    }
    return occlusion;
}

float computeHbao(uint2 outputPixel)
{
    float radius = aoParameters.x;
    if (aoFilter.w == 0.0 || radius <= 0.0 || !isfinite(radius) ||
        any(aoSizes.xy <= 0.0) || any(aoSizes.zw <= 0.0)) return 1.0;
    float radiusSquared = radius * radius;
    if (!isfinite(radiusSquared) || radiusSquared <= HBAO_EPSILON * HBAO_EPSILON) return 1.0;

    // Both the PS and CS pass the integer pixel index used by the reference.
    float2 inverseSize = rcp(aoSizes.xy);
    float2 outputUv = float2(outputPixel) * inverseSize;
    float3 center;
    if (!hbaoReadPosition(outputUv, center) || center.z > HBAO_DISCARD_DISTANCE) return 1.0;

    // ClipInfo in the reference stores cot(verticalFov / 2) for both axes.
    float radiusPixels = (0.5 * abs(aoProjection[1][1]) * radius / center.z) * aoSizes.x;
    if (!isfinite(radiusPixels) || radiusPixels <= 1.0) return 1.0;
    float3 dx = hbaoPositionDifference(outputUv, float2(inverseSize.x, 0.0), center);
    float3 dy = hbaoPositionDifference(outputUv, float2(0.0, inverseSize.y), center) * (aoSizes.y / aoSizes.x);

    // Match the reference's direct use of sampled XY as the rotation pair.
    float3 noise = aoRandomMap.SampleLevel(aoLinearWrap, outputUv * (aoSizes.xy * 0.25), 0).rgb;
    int steps = int(min(float(HBAO_STEPS), radiusPixels));
    float stepPixels = radiusPixels / (float(steps) + 1.0);
    float maxSteps = HBAO_MAX_RADIUS_PIXELS / stepPixels;
    if (maxSteps < steps)
    {
        steps = max(int(floor(maxSteps + noise.z)), 1);
        stepPixels = HBAO_MAX_RADIUS_PIXELS / float(steps);
    }
    float2 stepUv = stepPixels * inverseSize;
    float occlusion = 0.0;
    [unroll]
    for (int directionIndex = 0; directionIndex < HBAO_DIRECTIONS; ++directionIndex)
    {
        float sine, cosine;
        sincos((2.0 * HBAO_PI / HBAO_DIRECTIONS) * directionIndex, sine, cosine);
        float2 direction = float2(cosine * noise.x - sine * noise.y,
                                  cosine * noise.y + sine * noise.x);
        occlusion += hbaoHorizon(outputUv, center, dx, dy, direction * stepUv,
                                 inverseSize, steps, noise.z, radiusSquared);
    }
    return saturate(1.0 - (occlusion / HBAO_DIRECTIONS * HBAO_INTENSITY));
}

#endif
