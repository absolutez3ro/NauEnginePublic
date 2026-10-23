// multi_gpu_fullscreen.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nau/3d/dag_drv3d.h"

namespace d3d::mgpu
{
    struct FullscreenProducerDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        const void* vertexBytecode = nullptr;
        size_t vertexBytecodeSize = 0;
        const void* pixelBytecode = nullptr;
        size_t pixelBytecodeSize = 0;
    };

    struct FullscreenProducer;

    enum class FullscreenResult
    {
        submitted,
        busy,
        failed
    };

    /** @brief Creates an MG-10 producer and MG-08/09 channel on the secondary device. Bytecode is copied by the PSO.
     * Call on the render thread; destroy before device recovery or driver shutdown. Destination must be a tracked
     * TEXFMT_R8G8B8A8 texture of the requested size. The shader receives four floats at pixel b0.
     */
    NAU_RENDER_EXPORT FullscreenProducer* createFullscreenProducer(const FullscreenProducerDesc& desc);
    /** @brief Draws and transfers a frame. busy preserves the destination; failure latches the MG-11 fallback. */
    NAU_RENDER_EXPORT FullscreenResult renderFullscreen(FullscreenProducer* producer, const float* constants, BaseTexture* destination);
    /** @brief Drains engine/channel work before releasing the pass and allocator. May wait on the CPU. */
    NAU_RENDER_EXPORT void destroyFullscreenProducer(FullscreenProducer* producer);
    /** @brief Nonblocking collection, also required when the sample is running its single-GPU path. */
    NAU_RENDER_EXPORT void collectFullscreenResources();
}  // namespace d3d::mgpu
