// multi_gpu_ssao_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

namespace d3d::mgpu
{
    /** @brief Optional render-thread observer. Retained with pending native work, including after retirement. */
    class ISsaoProfiler
    {
    public:
        ISsaoProfiler() = default;
        ISsaoProfiler(const ISsaoProfiler&) = delete;
        ISsaoProfiler(ISsaoProfiler&&) = delete;
        ISsaoProfiler& operator=(const ISsaoProfiler&) = delete;
        ISsaoProfiler& operator=(ISsaoProfiler&&) = delete;
        virtual ~ISsaoProfiler() = default;

        virtual void attach(void* device, void* queue) = 0;
        virtual void beginPass(void* list, const char* name) = 0;
        virtual void endPass() = 0;
        virtual void resolve(void* list) = 0;
        virtual void submitted() = 0;
        virtual void detach() = 0;
    };

    /** @brief Column-major matrices and parameters matching ssao_common.hlsli exactly. */
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
