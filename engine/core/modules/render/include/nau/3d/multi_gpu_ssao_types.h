// multi_gpu_ssao_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <EASTL/shared_ptr.h>

#include "nau/3d/multi_gpu_transfer_types.h"

namespace d3d::mgpu
{
    /**
     * @brief Render-thread observer retained until pending native work completes, including after retirement.
     */
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
        virtual eastl::shared_ptr<ITransferProfiler> createTransferProfiler(const char* name, bool fromPrimary) = 0;
    };

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
