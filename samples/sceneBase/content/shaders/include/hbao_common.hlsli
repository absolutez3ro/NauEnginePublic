// Copyright 2026 N-GINN LLC. All rights reserved.
// Horizon search adapted from Bogdanov M.K., Shaders/Common/HBAO.hlsl, d17e45bc.
// Shared by the primary pixel path and the secondary compute path.
#ifndef NAU_HBAO_COMMON
#define NAU_HBAO_COMMON

#include "ssao_common.hlsli"

static const float HBAO_PI = 3.14159265359;
static const float HBAO_TAN_BIAS = 0.57735026919; // tan(30 degrees).
static const float HBAO_MAX_RADIUS_PIXELS = 64.0; // Input-depth texels, also for reduced AO.
static const float HBAO_INTENSITY = 1.9;
static const float HBAO_EPSILON = 1e-6;

bool hbaoReadPosition(int2 pixel, out float3 position)
{
    position = 0.0;
    if (any(pixel < 0) || any(pixel >= int2(aoSizes.zw))) return false;
    // rawDepth converts an optional positive linear R16F value back to reversed-Z.
    float depth = depthAtPixel(pixel);
    if (depth <= 0.0 || !isfinite(depth)) return false;
    float2 uv = (float2(pixel) + 0.5) / aoSizes.zw;
    float4 p = mul(aoInverseProjection, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
    if (abs(p.w) <= HBAO_EPSILON) return false;
    position = p.xyz / p.w;
    // Nau's right-handed view space looks down -Z; reject the eye and invalid input.
    return all(isfinite(position)) && position.z < -HBAO_EPSILON;
}

// One-input-pixel derivative in the positive texture axis. Prefer the shorter
// valid side at discontinuities; an unavailable neighbor never samples the sky.
float3 hbaoPositionDifference(int2 pixel, int2 axis, float3 center)
{
    float3 before, after;
    bool hasBefore = hbaoReadPosition(pixel - axis, before);
    bool hasAfter = hbaoReadPosition(pixel + axis, after);
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
    // +delta.z points toward the eye in Nau, therefore raises the occluding horizon.
    return delta.z * rsqrt(max(dot(delta.xy, delta.xy), HBAO_EPSILON * HBAO_EPSILON));
}

float hbaoTangentToSine(float tangent)
{
    return tangent * rsqrt(1.0 + tangent * tangent);
}

float hbaoHorizon(int2 centerPixel, float3 center, float3 dx, float3 dy,
                  float2 direction, float stepPixels, uint steps, float jitter, float inverseRadiusSquared)
{
    // Use actual sampled pixel centers for both the tangent and the horizon.
    // Texture Y points down; dy was reconstructed in this same coordinate system.
    float3 tangent = direction.x * dx + direction.y * dy;
    float tangentHorizon = hbaoTangent(tangent) + HBAO_TAN_BIAS;
    float sineHorizon = hbaoTangentToSine(tangentHorizon);
    float occlusion = 0.0;
    [unroll]
    for (uint step = 0; step < 4; ++step)
    {
        if (step >= steps) break;
        int2 samplePixel = centerPixel + int2(round(direction * ((float(step) + 1.0 + jitter) * stepPixels)));
        float3 samplePosition;
        if (!hbaoReadPosition(samplePixel, samplePosition)) continue;
        float3 delta = samplePosition - center;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared <= HBAO_EPSILON * HBAO_EPSILON || distanceSquared * inverseRadiusSquared >= 1.0) continue;
        float sampleTangent = hbaoTangent(delta);
        if (sampleTangent > tangentHorizon)
        {
            float sampleSine = hbaoTangentToSine(sampleTangent);
            occlusion += saturate(1.0 - distanceSquared * inverseRadiusSquared) * (sampleSine - sineHorizon);
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

    // PS SV_Position.xy and CS (DTid.xy + 0.5) refer to identical output centers.
    // Map once to an input texel, then reconstruct at that texel's actual center.
    float2 outputUv = (float2(outputPixel) + 0.5) / aoSizes.xy;
    int2 centerPixel = min(int2(outputUv * aoSizes.zw), int2(aoSizes.zw) - 1);
    float3 center;
    if (!hbaoReadPosition(centerPixel, center)) return 1.0;

    // Project scene-unit radius onto the input-depth grid. The cap is applied
    // before step distribution so the jittered final tap remains within 64 pixels.
    float radiusPixels = 0.5 * abs(aoProjection[0][0]) * radius * aoSizes.z / -center.z;
    if (!isfinite(radiusPixels) || radiusPixels <= 1.0) return 1.0;
    radiusPixels = min(radiusPixels, HBAO_MAX_RADIUS_PIXELS);
    uint steps = min(4u, uint(floor(radiusPixels)));
    float stepPixels = radiusPixels / (float(steps) + 1.0);
    float3 dx = hbaoPositionDifference(centerPixel, int2(1, 0), center);
    float3 dy = hbaoPositionDifference(centerPixel, int2(0, 1), center);

    // Nau's existing random texture is RGB UNORM, unlike a signed cos/sin map.
    // Decode and normalize XY for rotation; Z remains a [0,1] step jitter.
    float3 noise = aoRandomMap.SampleLevel(aoLinearWrap, (float2(outputPixel) + 0.5) * 0.25, 0).rgb;
    float2 rotation = noise.xy * 2.0 - 1.0;
    float rotationLengthSquared = dot(rotation, rotation);
    rotation = rotationLengthSquared > HBAO_EPSILON ? rotation * rsqrt(rotationLengthSquared) : float2(1.0, 0.0);
    float jitter = saturate(noise.z);
    // The existing eightSamples toggle means 8 directions for HBAO; otherwise 16.
    // Each direction has up to four horizon steps, independent of SSAO's 8/14 taps.
    uint directions = aoOptions.w <= 8.0 ? 8u : 16u;
    float occlusion = 0.0;
    [unroll]
    for (uint directionIndex = 0; directionIndex < 16; ++directionIndex)
    {
        if (directionIndex >= directions) break;
        float sine, cosine;
        sincos((2.0 * HBAO_PI / float(directions)) * float(directionIndex), sine, cosine);
        float2 direction = float2(cosine * rotation.x - sine * rotation.y,
                                  cosine * rotation.y + sine * rotation.x);
        occlusion += hbaoHorizon(centerPixel, center, dx, dy, direction, stepPixels, steps, jitter, rcp(radiusSquared));
    }
    return saturate(1.0 - HBAO_INTENSITY * occlusion / float(directions));
}

#endif
