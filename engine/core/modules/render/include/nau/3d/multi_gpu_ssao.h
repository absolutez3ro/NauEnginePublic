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
    /** @brief Describes the AO shaders, resource dimensions and transport for GPU1. */
    struct SsaoProducerDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t inputWidth = 0;   ///< Zero uses the AO output width.
        uint32_t inputHeight = 0;  ///< Zero uses the AO output height.
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
        const void* xePrefilterBytecode = nullptr;
        size_t xePrefilterBytecodeSize = 0;
        const void* xeMainBytecode = nullptr;
        size_t xeMainBytecodeSize = 0;
        const void* xeDenoiseBytecode = nullptr;
        size_t xeDenoiseBytecodeSize = 0;
        const void* xeDenoiseLastBytecode = nullptr;
        size_t xeDenoiseLastBytecodeSize = 0;
        const void* xeCompositeBytecode = nullptr;
        size_t xeCompositeBytecodeSize = 0;
        const void* filterBytecode = nullptr;
        size_t filterBytecodeSize = 0;
        bool reconstructNormals = false;
        bool packedNormals = false;
        bool linearDepth16 = false;
        bool measureTimings = false;
        bool frameworkStyle = false;
        bool mailboxStyle = false;
        bool gpuMailbox = true;
        MailboxDiagnostic mailboxDiagnostic = MailboxDiagnostic::disabled;
    };

    struct SsaoProducer;
    /**
     * @brief Creates a mailbox, shared-texture or protected AO producer on the render thread.
     * @param desc Shader bytecode, resource dimensions and transport settings.
     * @return A producer owned by the caller, or nullptr if creation fails. Destroy it with destroySsaoProducer().
     */
    NAU_RENDER_EXPORT SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc);
    /**
     * @brief Queues RG16F/R32F inputs at their declared size, raw R8 AO, and its return copy.
     * Keep endpoints alive until destruction or resize drains both devices. The caller records
     * wait_for_primary_transfer() before filtering destination, after its independent graphics work,
     * or consumes a completed destination after is_primary_transfer_complete() returns true.
     * @param producer Protected producer.
     * @param constants AO settings and matrices for the submitted frame.
     * @param normals Input normals, unused when reconstructed from depth.
     * @param depth Input depth texture.
     * @param destination Primary-device R8 AO texture.
     * @param frame Source frame ID for profiling.
     * @return Completion point for the returned AO, or a null fence on failure. This call never waits on the CPU.
     */
    NAU_RENDER_EXPORT PrimarySyncPoint submitSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame = 0);
    /**
     * @brief Experimental unsynchronized shared-texture exchange, recorded in the primary graphics list.
     * Always publishes inputs and reads shared AO, irrespective of GPU1 readiness. GPU1 copies/computes/
     * publishes in one direct list when one of its three command recordings is free. No intermediate
     * submit, copy queue or cross-adapter fence is used. Concurrent read/write races are intentional.
     * @param producer Shared-texture producer.
     * @param constants AO settings and matrices for the submitted frame.
     * @param normals Input normals, unused when reconstructed from depth.
     * @param depth Input depth texture.
     * @param destination Persistent primary-device AO texture, initially cleared to white by the caller.
     * @param frame Source frame ID for profiling.
     * @param[out] submitted Whether a GPU1 list was queued.
     * @return True while the transport remains usable, even when all GPU1 recordings are busy.
     */
    NAU_RENDER_EXPORT bool exchangeFrameworkSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame, bool& submitted);
    /**
     * @brief Submits frame inputs early, before the primary GPU's independent graphics passes.
     * Busy slots or backlog skip submission without waiting for GPU1 on the CPU or GPU0.
     * @param producer Mailbox producer.
     * @param constants AO settings and matrices associated with the inputs.
     * @param normals Input normals, unused when reconstructed from depth.
     * @param depth Input depth texture.
     * @param frame Nonzero source frame ID that fits in 32 bits.
     * @param[out] submitted Whether a GPU1 AO job was queued.
     * @param freezeInputsReady Allows the frozen-input diagnostic to pin a fully warmed-up frame.
     * @return True while the transport remains usable, including when submission was skipped.
     */
    NAU_RENDER_EXPORT bool submitMailboxSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, uint64_t frame, bool& submitted, bool freezeInputsReady = false);
    /**
     * @brief Records selection and copy of the latest published AO before lighting resolve.
     * Retains the previous result when no newer one is available. This call does not wait for GPU1.
     * @warning GPU selection reads mailbox metadata concurrently with GPU1 publication.
     * @param producer Mailbox producer.
     * @param destination Persistent primary-device AO texture.
     * @param frame Consumer frame ID used for AO age readback.
     * @return True if the transport remains usable. It may retain the previous result.
     */
    NAU_RENDER_EXPORT bool receiveMailboxSsao(SsaoProducer* producer, BaseTexture* destination, uint64_t frame);
    /**
     * @brief Polls completed GPU1 timings and mailbox observations without waiting.
     * @param producer Producer to sample. May be nullptr.
     * @return Latest snapshot. A zero timing frame means GPU1 timestamps are unavailable.
     */
    NAU_RENDER_EXPORT SsaoGpuTiming pollSsaoTiming(SsaoProducer* producer);
    /**
     * @brief Destroys the producer and retires native resources before endpoint destruction, resize or driver shutdown.
     * @param producer Producer returned by createSsaoProducer(), or nullptr.
     */
    NAU_RENDER_EXPORT void destroySsaoProducer(SsaoProducer* producer);
    /** @brief Collects retired native resources on the render thread without waiting. */
    NAU_RENDER_EXPORT void collectSsaoResources();
}  // namespace d3d::mgpu
