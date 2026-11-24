// multi_gpu_ssao_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once
#include <cstdint>

namespace d3d::mgpu
{
    /**
     * @brief AO timing and mailbox counters collected from different queues.
     * Queue-local intervals must not be added to infer end-to-end latency. Negative intervals are unavailable.
     */
    struct SsaoGpuTiming
    {
        uint64_t frame = 0;
        float rawMilliseconds = -1.f;
        float totalMilliseconds = -1.f;  ///< Raw AO plus blur on GPU1.
        float inputMilliseconds = -1.f;  ///< Input copy and optional unpack measured by the shared-texture transport.
        float blurMilliseconds = -1.f;
        float outputMilliseconds = -1.f;
        float jobMilliseconds = -1.f;
        uint64_t submittedJobs = 0;
        uint64_t completedJobs = 0;
        uint64_t skippedSubmissions = 0;
        uint64_t primaryFrame = 0;
        float primaryPublishMilliseconds = -1.f;
        float primaryReceiveMilliseconds = -1.f;
        bool packedDepth = false;
        bool packedNormals = false;
        bool mailbox = false;
        bool gpuMailbox = false;
        uint64_t displayedFrame = 0, displayedAoFrame = 0, ageSamples = 0;
        uint64_t ageHistogram[8] = {};  ///< Bins 0 to 6 frames, then 7 or more, from GPU-copied source IDs.
        uint64_t skippedInput = 0, skippedOutput = 0, skippedBacklog = 0;
        uint64_t submitOnlySignals = 0, badFrameIds = 0, unavailableAoFrames = 0, droppedAgeSamples = 0;
        uint64_t primaryInputCopyFrames = 0;
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
