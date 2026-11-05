// multi_gpu_ssao_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once
#include <cstdint>

namespace d3d::mgpu
{
    struct SsaoGpuTiming
    {
        uint64_t frame = 0;
        float rawMilliseconds = -1.f;
        float totalMilliseconds = -1.f;
        // GPU1's own queue intervals. totalMilliseconds retains its raw + blur meaning.
        // Input includes unpack when enabled; these copy intervals are available only for framework transport.
        float inputMilliseconds = -1.f;
        float blurMilliseconds = -1.f;
        float outputMilliseconds = -1.f;
        float jobMilliseconds = -1.f;
        uint64_t submittedJobs = 0;
        uint64_t completedJobs = 0;
        uint64_t skippedSubmissions = 0;
        // GPU0 intervals are measured on its queue and must not be summed to infer end-to-end latency.
        uint64_t primaryFrame = 0;
        float primaryPublishMilliseconds = -1.f;
        float primaryReceiveMilliseconds = -1.f;
        bool packedDepth = false;
        bool packedNormals = false;
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
        float options[4] = {0.0f, 0.0f, 0.0f, 14.0f};
        float pipeline[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float sizes[4] = {};
        float reconstruction[4] = {};
    };
    static_assert(sizeof(SsaoConstants) == 288);
}  // namespace d3d::mgpu
