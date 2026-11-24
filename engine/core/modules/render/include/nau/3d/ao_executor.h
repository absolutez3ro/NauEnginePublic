// ao_executor.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "nau/3d/ao_constants.h"
#include "nau/3d/ao_execution_stats.h"
#include "nau/3d/dag_drv3d.h"

namespace nau::render::ao
{
    class NativeAlgorithm;
    using NativeAlgorithmFactory = NativeAlgorithm* (*)();
    struct ShaderBytecode
    {
        const void* data = nullptr;
        size_t size = 0;
    };
    /** @brief Describes the AO shaders and common input/output dimensions for GPU1. */
    struct ExecutorDescription
    {
        uint32_t width = 0;
        uint32_t height = 0;
        const void* vertexBytecode = nullptr;
        size_t vertexBytecodeSize = 0;
        const void* rawBytecode = nullptr;
        size_t rawBytecodeSize = 0;
        /**
         * @brief Optional 8x8 compute pass with the same constants and SRVs as the pixel pass.
         * Writes R8 AO at u0. Devices without typed R8 UAV stores use the pixel pass.
         */
        const void* rawComputeBytecode = nullptr;
        size_t rawComputeBytecodeSize = 0;
        NativeAlgorithmFactory createAlgorithm = nullptr;
        ShaderBytecode algorithmShaders[8];
        uint32_t algorithmShaderCount = 0;
        bool reconstructNormals = false;
        bool measureTimings = false;
    };

    struct SecondaryExecutor;
    /**
     * @brief Creates a mailbox 2b producer on the render thread.
     * @param desc Shader bytecode, resource dimensions and input requirements.
     * @return A producer owned by the caller, or nullptr if creation fails. Destroy it with destroySecondaryExecutor().
     */
    NAU_RENDER_EXPORT SecondaryExecutor* createSecondaryExecutor(const ExecutorDescription& desc);
    /**
     * @brief Submits frame inputs early, before the primary GPU's independent graphics passes.
     * Busy slots or backlog skip submission without waiting for GPU1 on the CPU or GPU0.
     * @param producer Mailbox producer.
     * @param constants AO settings and matrices associated with the inputs.
     * @param normals Input normals, unused when reconstructed from depth.
     * @param depth Input depth texture.
     * @param frame Nonzero source frame ID that fits in 32 bits.
     * @return True while the transport remains usable, including when submission was skipped.
     */
    NAU_RENDER_EXPORT bool submitSecondary(SecondaryExecutor* producer, const nau::render::AoConstants& constants, BaseTexture* normals, BaseTexture* depth, uint64_t frame);
    /**
     * @brief Records selection and copy of the latest published AO before lighting resolve.
     * Retains the previous result when no newer one is available. This call does not wait for GPU1.
     * @warning GPU selection reads mailbox metadata concurrently with GPU1 publication.
     * @param producer Mailbox producer.
     * @param destination Persistent primary-device AO texture.
     * @param frame Consumer frame ID used for AO age readback.
     * @return True if the transport remains usable. It may retain the previous result.
     */
    NAU_RENDER_EXPORT bool receiveSecondary(SecondaryExecutor* producer, BaseTexture* destination, uint64_t frame);
    /**
     * @brief Polls completed GPU1 timings and mailbox observations without waiting.
     * @param producer Producer to sample. May be nullptr.
     * @return Latest snapshot. A zero timing frame means GPU1 timestamps are unavailable.
     */
    NAU_RENDER_EXPORT ExecutionStats pollSecondary(SecondaryExecutor* producer);
    /**
     * @brief Destroys the producer and retires native resources before endpoint destruction, resize or driver shutdown.
     * @param producer Producer returned by createSecondaryExecutor(), or nullptr.
     */
    NAU_RENDER_EXPORT void destroySecondaryExecutor(SecondaryExecutor* producer);
    /** @brief Collects retired native resources on the render thread without waiting. */
    NAU_RENDER_EXPORT void collectSecondaryResources();
}  // namespace nau::render::ao
