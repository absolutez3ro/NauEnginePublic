// ao_constants.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once
namespace nau::render
{
    /**
     * @brief Column-major matrices and parameters matching ssao_common.hlsli.
     */
    struct AoConstants
    {
        float projection[16] = {};
        float inverseProjection[16] = {};
        float view[16] = {};
        float parameters[4] = {0.5f, 0.2f, 1.0f, 0.05f};
        float filter[4] = {0.0f, 0.0f, 0.2f, 1.0f};
        float options[4] = {0.0f, 0.0f, 0.0f, 14.0f};
        float pipeline[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float sizes[4] = {};
        float reconstruction[4] = {};
    };
    static_assert(sizeof(AoConstants) == 288);
}  // namespace nau::render
