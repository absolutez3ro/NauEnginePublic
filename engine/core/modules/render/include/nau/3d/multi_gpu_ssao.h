// multi_gpu_ssao.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <EASTL/shared_ptr.h>

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
        eastl::shared_ptr<ISsaoProfiler> profiler;
        const void* normalBytecode = nullptr;
        size_t normalBytecodeSize = 0;
        bool depthIsFloat = false;  // R32F reduction input; false preserves the DEPTH32 input contract.
    };

    struct SsaoProducer;
    enum class SsaoResult
    {
        submitted,
        busy,
        failed
    };

    /**
     * @brief Creates a GPU 1 pass and its channels using Nau's heap pool. Call on the render thread.
     */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /**
     * @brief Checks for an outstanding job or busy command slot before preparing inputs; never waits on the CPU.
     */
    NAU_RENDER_EXPORT bool isSsaoBusy(const SsaoProducer* producer);
    /**
     * @brief Sends inputs and schedules SSAO on GPU 1, retaining its output ticket across frames.
     * Only one job is outstanding. busy preserves that job and schedules no new work.
     * captureRaw requests an additional R32F validation copy.
     * When normalBytecode is supplied, GPU 1 reconstructs normals from depth; normals may be nullptr.
     */
    NAU_RENDER_EXPORT SsaoResult submitSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, bool captureRaw = false);
    /**
     * @brief Receives a previously submitted result only after its source copy is complete.
     * busy leaves destinations unchanged and records no primary queue wait. Call before submitting the next frame.
     */
    NAU_RENDER_EXPORT SsaoResult tryReceiveSsao(SsaoProducer* producer, BaseTexture* destination, BaseTexture* rawDestination = nullptr);
    /**
     * @brief Compatibility path: computes SSAO and immediately schedules the primary GPU to wait for its result.
     * Inputs: TEXFMT_G16R16F sphere-map normals, TEXFMT_DEPTH32 depth. Output: TEXFMT_R8, all of desc's size.
     * busy submits no complete result; the caller may display its last valid AO map or compute on GPU 0.
     * rawDestination is an optional TEXFMT_R32F validation-only copy of the unfiltered GPU 1 result.
     */
    NAU_RENDER_EXPORT SsaoResult renderSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, BaseTexture* rawDestination = nullptr);
    /**
     * @brief Drains channels before releasing resources. Call before driver shutdown and on resize.
     */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
