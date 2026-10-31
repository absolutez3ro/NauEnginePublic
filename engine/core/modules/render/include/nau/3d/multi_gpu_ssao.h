// multi_gpu_ssao.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau/3d/multi_gpu_ssao_types.h"

namespace d3d::mgpu
{
    struct SsaoProducerDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        const void* vertexBytecode = nullptr;
        size_t vertexBytecodeSize = 0;
        const void* rawBytecode = nullptr;
        size_t rawBytecodeSize = 0;
    };

    struct SsaoProducer;
    /** @brief Creates a raw half-resolution SSAO producer with three synchronized shared slots. */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /**
     * @brief Queues RG16F/R32F inputs, raw R8 AO, and the return copy for this frame.
     * Keep endpoints alive until destruction or resize drains both devices. The caller records
     * wait_for_primary_transfer() before filtering destination, after its independent graphics work.
     * A null fence reports failure; no readiness polling or CPU wait is performed.
     */
    NAU_RENDER_EXPORT PrimarySyncPoint submitSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination);
    /** @brief Drains transfers and retires native resources before endpoint destruction, resize or driver shutdown. */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    /** @brief Collects retired native resources on the render thread without waiting. */
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
