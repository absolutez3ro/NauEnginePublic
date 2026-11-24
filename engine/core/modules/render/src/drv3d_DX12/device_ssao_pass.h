// device_ssao_pass.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <EASTL/shared_ptr.h>
#include <EASTL/unique_ptr.h>

#include "device_resource.h"
#include "multi_gpu_transfer.h"
#include "nau/3d/multi_gpu_ssao_types.h"

namespace drv3d_dx12
{
    struct DeviceSsaoPassDesc
    {
        eastl::shared_ptr<IDeviceResourceAllocator> allocator;
        UINT width = 0;
        UINT height = 0;
        UINT inputWidth = 0;
        UINT inputHeight = 0;
        D3D12_SHADER_BYTECODE vertexShader = {};
        D3D12_SHADER_BYTECODE rawShader = {};
        D3D12_SHADER_BYTECODE rawComputeShader = {};
        D3D12_SHADER_BYTECODE xePrefilterShader = {};
        D3D12_SHADER_BYTECODE xeMainShader = {};
        D3D12_SHADER_BYTECODE xeDenoiseShader = {};
        D3D12_SHADER_BYTECODE xeDenoiseLastShader = {};
        D3D12_SHADER_BYTECODE xeCompositeShader = {};
        bool reconstructNormals = false;
        bool packedNormals = false;
        bool linearDepth16 = false;
        bool measureTimings = false;
        bool packedFrameworkDepth = false;
        bool packedFrameworkNormals = false;
    };

    /** @brief Shared textures as seen by GPU1. */
    struct MailboxSsaoTextures
    {
        ID3D12Resource* normals = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* result = nullptr;
    };

    /** @brief Input dependency and output resources for a native GPU1 mailbox job. */
    struct MailboxSsaoJob
    {
        CrossAdapterSyncPoint inputsReady;
        CrossAdapterFence* completion = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* mailbox = nullptr;
        uint32_t outputSlot = 0;
    };

    struct MailboxSsaoCompletion
    {
        uint64_t dataReady = 0;  ///< Completion fence value for AO and its frame ID.
        uint64_t published = 0;  ///< Completion fence value including mailbox metadata publication.
    };

    /** @brief Native GPU1 AO compute and publication for mailbox 2b. */
    class DeviceSsaoPass final
    {
    public:
        DeviceSsaoPass();
        DeviceSsaoPass(const DeviceSsaoPass&) = delete;
        DeviceSsaoPass(DeviceSsaoPass&&) = delete;
        DeviceSsaoPass& operator=(const DeviceSsaoPass&) = delete;
        DeviceSsaoPass& operator=(DeviceSsaoPass&&) = delete;
        ~DeviceSsaoPass();

        HRESULT init(const DeviceSsaoPassDesc& desc);
        /**
         * @brief Waits for native submissions before releasing the producer's resources.
         * @param timeoutMs Maximum wait in milliseconds.
         * @return S_OK on success. On timeout the pass retains its resources for another reset attempt.
         */
        HRESULT reset(DWORD timeoutMs = 10000);
        /**
         * @brief Queues GPU1 AO after the input dependency, then publishes its output.
         * @param constants AO settings and matrices for the input data.
         * @param shared Input and output textures as opened on GPU1.
         * @param job Fence and metadata resources for the assigned mailbox slots.
         * @param frame Source frame ID written alongside the output.
         * @param[out] completion Fence values for the output and its publication.
         * @return S_OK when queued, S_FALSE when no recording is free, or a failure HRESULT.
         */
        HRESULT executeMailbox(const d3d::mgpu::SsaoConstants& constants, const MailboxSsaoTextures& shared, const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion);
        void retainMailboxResources(const eastl::shared_ptr<void>& resources);
        d3d::mgpu::SsaoGpuTiming pollTiming();
        /**
         * @brief Releases retired GPU1 resources whose work has completed, without blocking.
         * @return Number of retired pass states still pending.
         */
        static size_t collectRetiredResources();

    private:
        class State;
        static State* s_retired;
        eastl::unique_ptr<State> m_state;
    };
}  // namespace drv3d_dx12
