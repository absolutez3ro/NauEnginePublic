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
        D3D12_SHADER_BYTECODE filterShader = {};
        bool reconstructNormals = false;
        bool packedNormals = false;
        bool linearDepth16 = false;
        bool measureTimings = false;
        bool frameworkStyle = false;
        bool mailboxStyle = false;
        bool packedFrameworkDepth = false;
        bool packedFrameworkNormals = false;
    };

    /** @brief Shared textures and backing heaps as seen by one adapter. */
    struct FrameworkSsaoTextures
    {
        ID3D12Resource* normals = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* result = nullptr;
        ID3D12Heap* heaps[3] = {};
    };

    /** @brief Input dependency and output resources for a native GPU1 mailbox job. */
    struct MailboxSsaoJob
    {
        CrossAdapterSyncPoint inputsReady;
        CrossAdapterFence* completion = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* mailbox = nullptr;
        uint32_t outputSlot = 0;
        bool publishMailbox = true;
        uint64_t profiledInputBytes = 0;
        bool profileInputTransfer = true;
    };

    struct MailboxSsaoCompletion
    {
        uint64_t dataReady = 0;  ///< Completion fence value for AO and its frame ID.
        uint64_t published = 0;  ///< Completion fence value including mailbox metadata publication.
    };

    /**
     * @brief Native GPU1 AO for the mailbox, shared-texture and protected transports.
     * Framework mode uses one local resource set and three command/constant recordings. It reads
     * shared inputs at execution, never waits for the other adapter and skips submission if all
     * three recordings are busy. Protected mode uses two resource sets and queue waits for their
     * inputs and output readers. Both paths use one direct queue and COMMON between submissions.
     */
    class DeviceSsaoPass final
    {
    public:
        static constexpr uint32_t RESOURCE_COUNT = 2;
        DeviceSsaoPass();
        DeviceSsaoPass(const DeviceSsaoPass&) = delete;
        DeviceSsaoPass(DeviceSsaoPass&&) = delete;
        DeviceSsaoPass& operator=(const DeviceSsaoPass&) = delete;
        DeviceSsaoPass& operator=(DeviceSsaoPass&&) = delete;
        ~DeviceSsaoPass();

        HRESULT init(const DeviceSsaoPassDesc& desc);
        /**
         * @brief Waits for native submissions and outstanding output readers before releasing resources.
         * @param timeoutMs Maximum wait in milliseconds.
         * @return S_OK on success. On timeout the pass retains its resources for another reset attempt.
         */
        HRESULT reset(DWORD timeoutMs = 10000);
        HRESULT execute(const d3d::mgpu::SsaoConstants& constants, CrossAdapterSyncPoint normalsReady = {}, CrossAdapterSyncPoint depthReady = {}, uint64_t frame = 0, uint32_t resourceIndex = 0);
        /**
         * @brief Copies shared inputs, computes AO and publishes the result in one direct list.
         * @param constants AO settings and matrices.
         * @param shared Shared textures as opened on GPU1.
         * @param frame Source frame ID for profiling.
         * @return S_OK when queued, S_FALSE when all three recordings are busy, or a failure HRESULT.
         */
        HRESULT executeFramework(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared, uint64_t frame = 0);
        /**
         * @brief Queues GPU1 AO after the input dependency, then publishes its output.
         * @param constants AO settings and matrices for the input data.
         * @param shared Input and output textures as opened on GPU1.
         * @param job Fence and metadata resources for the assigned mailbox slots.
         * @param frame Source frame ID written alongside the output.
         * @param[out] completion Fence values for the output and its publication.
         * @return S_OK when queued, S_FALSE when no recording is free, or a failure HRESULT.
         */
        HRESULT executeMailbox(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared, const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion);
        void retainMailboxResources(const eastl::shared_ptr<void>& resources);
        d3d::mgpu::SsaoGpuTiming pollTiming();
        /**
         * @brief Records the last reader of an output resource set.
         * The dependency is applied when the set is rendered again or the pass is reset.
         * @param released Fence point after the reader finishes.
         * @param resourceIndex Index of the resource set.
         * @return S_OK on success, or a failure HRESULT for an invalid or already pending release.
         */
        HRESULT releaseOutput(CrossAdapterSyncPoint released, uint32_t resourceIndex = 0);
        CrossAdapterSyncPoint getReadyPoint(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getNormals(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getDepth(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getResult(uint32_t resourceIndex = 0) const;
        /** @brief Returns the native queue borrowed for the lifetime of the initialized pass. */
        ID3D12CommandQueue* getQueue() const;
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
