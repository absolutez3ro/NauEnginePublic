// multi_gpu_ssao_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

namespace d3d::mgpu
{
    /**
     * @brief Column-major matrices and parameters matching ssao_common.hlsli.
     */
    struct SsaoConstants
    {
        float projection[16] = {};
        float inverseProjection[16] = {};
        float view[16] = {};
        float parameters[4] = {0.5f, 0.2f, 1.0f, 0.05f};
        float filter[4] = {0.0f, 0.0f, 0.2f, 1.0f};
    };
    static_assert(sizeof(SsaoConstants) == 224);
}  // namespace d3d::mgpu
