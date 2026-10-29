// multi_gpu_ssao.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "nau/3d/dag_drv3d.h"
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
        const void* blurBytecode = nullptr;
        size_t blurBytecodeSize = 0;
    };

    struct SsaoProducer;
    enum class SsaoResult
    {
        submitted,
        busy,
        failed
    };

    /** @brief Creates full-resolution SSAO with three-slot input/output channels on the render thread. */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /** @brief Polls pending work and input-copy leases. Never waits on the CPU or primary graphics queue. */
    NAU_RENDER_EXPORT bool isSsaoBusy(const SsaoProducer* producer);
    /**
     * @brief Sends persistent full-resolution G-buffer snapshots (RG32F sphere-map normals, DEPTH32 depth).
     * Keep both snapshots alive and unchanged until isSsaoBusy() returns false, even after busy/failed.
     * One job is pending at a time; partial copies retain their leases. No primary queue wait is recorded.
     */
    NAU_RENDER_EXPORT SsaoResult submitSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth);
    /**
     * @brief Polls GPU 1 output, starts a primary copy, then publishes only after that copy completes.
     * destination must remain alive, unchanged and unread across busy returns. Always pass the same
     * destination until submitted is returned. No primary graphics queue wait is recorded.
     */
    NAU_RENDER_EXPORT SsaoResult tryReceiveSsao(SsaoProducer* producer, BaseTexture* destination);
    /** @brief Drains transfers and retires native resources before endpoint destruction, resize or driver shutdown. */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    /** @brief Collects retired native resources on the render thread without waiting. */
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
