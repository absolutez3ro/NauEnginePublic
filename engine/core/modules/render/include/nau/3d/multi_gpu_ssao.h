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
        uint32_t inputWidth = 0;  // Zero uses the AO output width.
        uint32_t inputHeight = 0; // Zero uses the AO output height.
        const void* vertexBytecode = nullptr;
        size_t vertexBytecodeSize = 0;
        const void* rawBytecode = nullptr;
        size_t rawBytecodeSize = 0;
        const void* filterBytecode = nullptr;
        size_t filterBytecodeSize = 0;
        bool reconstructNormals = false;
        bool packedNormals = false;
        bool linearDepth16 = false;
        bool measureTimings = false;
        bool frameworkStyle = false;
    };

    struct SsaoProducer;
    /** @brief Creates the framework shared-texture producer when frameworkStyle is set, or the protected comparison producer otherwise. */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /**
     * @brief Queues RG16F/R32F inputs at their declared size, raw R8 AO, and its return copy.
     * Keep endpoints alive until destruction or resize drains both devices. The caller records
     * wait_for_primary_transfer() before filtering destination, after its independent graphics work,
     * or consumes a completed destination after is_primary_transfer_complete() returns true.
     * A null fence reports failure; no readiness polling or CPU wait is performed.
     */
    NAU_RENDER_EXPORT PrimarySyncPoint submitSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame = 0);
    /**
     * @brief Experimental unsynchronized shared-texture exchange, recorded in the primary graphics list.
     * Always publishes inputs and reads shared AO, irrespective of GPU1 readiness. GPU1 copies/computes/
     * publishes in one direct list when one of its three command recordings is free. No intermediate
     * submit, copy queue or cross-adapter fence is used. Concurrent read/write races are intentional.
     * True means the transport remains usable; submitted reports whether a GPU1 list was queued.
     */
    NAU_RENDER_EXPORT bool exchangeFrameworkSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame, bool& submitted);
    /** @brief Latest completed GPU 1 timestamps, read without waiting; frame zero means unavailable. */
    NAU_RENDER_EXPORT SsaoGpuTiming pollSsaoTiming(SsaoProducer* producer);
    /** @brief Drains transfers and retires native resources before endpoint destruction, resize or driver shutdown. */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    /** @brief Collects retired native resources on the render thread without waiting. */
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
