// Copyright 2026 N-GINN LLC. All rights reserved.
// Adapted from Bogdanov M.K.'s MGPU-SSAO XeGTAO passes, d17e45bc.
#ifndef NAU_XEGTAO_COMMON
#define NAU_XEGTAO_COMMON

#define XE_GTAO_FP32_DEPTHS 1
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 1
#define XE_GTAO_USE_DEFAULT_CONSTANTS 1
#define NAU_XEGTAO_SOURCE_DEPTH 1
#define VA_SATURATE(x) saturate(x)

#include "xegtao/XeGTAO.h"

// This layout is shared with SsaoConstants for all AO algorithms and transports.
cbuffer AmbientOcclusionConstants : register(b0)
{
    float4x4 aoProjection;
    float4x4 aoInverseProjection;
    float4x4 aoView;
    float4 aoParameters;
    float4 aoFilter;
    float4 aoOptions;
    float4 aoPipeline;
    float4 aoSizes;
    float4 aoReconstruction;
};

SamplerState xePointClamp : register(s0);

// Gather order matches Intel's 2x2 prefilter. Preserve the native gather when the
// input grid matches, and map output pixel centers when full-size inputs are used.
float4 NauXeGatherDepth(Texture2D<float> depth, SamplerState depthSampler,
                       uint2 pixel, GTAOConstants constants)
{
    if (all(aoSizes.xy == aoSizes.zw))
        return depth.GatherRed(depthSampler, pixel * constants.ViewportPixelSize, int2(1, 1));
    float2 inverseSize = constants.ViewportPixelSize;
    float topLeft = depth.SampleLevel(depthSampler, (pixel + float2(0.5, 0.5)) * inverseSize, 0);
    float topRight = depth.SampleLevel(depthSampler, (pixel + float2(1.5, 0.5)) * inverseSize, 0);
    float bottomLeft = depth.SampleLevel(depthSampler, (pixel + float2(0.5, 1.5)) * inverseSize, 0);
    float bottomRight = depth.SampleLevel(depthSampler, (pixel + float2(1.5, 1.5)) * inverseSize, 0);
    return float4(bottomLeft, bottomRight, topRight, topLeft);
}

#include "xegtao/XeGTAO.hlsli"

GTAOConstants loadXeGtaoConstants()
{
    GTAOConstants constants = (GTAOConstants)0;
    constants.ViewportSize = int2(aoSizes.xy);
    constants.ViewportPixelSize = rcp(aoSizes.xy);
    // Reflect Nau RH view Z into the positive-Z coordinates expected by XeGTAO.
    constants.DepthUnpackConsts = float2(-aoProjection[2][3], -aoProjection[2][2]);
    constants.CameraTanHalfFOV = rcp(float2(aoProjection[0][0], aoProjection[1][1]));
    constants.NDCToViewMul = constants.CameraTanHalfFOV * float2(2.0, -2.0);
    constants.NDCToViewAdd = constants.CameraTanHalfFOV * float2(-1.0, 1.0);
    constants.NDCToViewMul_x_PixelSize = constants.NDCToViewMul * constants.ViewportPixelSize;
    constants.EffectRadius = aoParameters.x;
    constants.EffectFalloffRange = XE_GTAO_DEFAULT_FALLOFF_RANGE;
    constants.RadiusMultiplier = XE_GTAO_DEFAULT_RADIUS_MULTIPLIER;
    constants.FinalValuePower = XE_GTAO_DEFAULT_FINAL_VALUE_POWER;
    constants.DenoiseBlurBeta = 1.2;
    constants.SampleDistributionPower = XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER;
    constants.ThinOccluderCompensation = XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION;
    constants.DepthMIPSamplingOffset = XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET;
    constants.NoiseIndex = 0;
    return constants;
}

#endif
