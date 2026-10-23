// multi_gpu_ssao.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
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

    /** @brief Render-thread API. Owns a GPU 1 pass and depth/normals/AO channels, using Nau's heap pool. */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /** @brief Moves this frame's inputs to GPU 1, computes SSAO and both blur passes, returns AO to GPU 0.
     * Inputs: TEXFMT_A32B32G32R32F normals, TEXFMT_DEPTH32 depth. Output: TEXFMT_R32F, all of desc's size.
     * busy leaves the caller responsible for computing THIS frame on GPU 0; never reuse an older AO map.
     * rawDestination is an optional validation-only copy of the unfiltered GPU 1 result.
     */
    NAU_RENDER_EXPORT SsaoResult renderSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, BaseTexture* rawDestination = nullptr);
    /** @brief Drains channels before releasing native resources; call before driver shutdown and on resize. */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
