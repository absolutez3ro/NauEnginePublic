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

    struct FrameworkSsaoTextures
    {
        ID3D12Resource* normals = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* result = nullptr;
        ID3D12Heap* heaps[3] = {};
    };

    struct MailboxSsaoJob
    {
        CrossAdapterSyncPoint inputsReady;
        CrossAdapterFence* completion = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* mailbox = nullptr;
        uint32_t outputSlot = 0;
        bool publishMailbox = true;
    };

    struct MailboxSsaoCompletion
    {
        uint64_t dataReady = 0;
        uint64_t published = 0;
    };

    /**
     * @brief Native SSAO on GPU1, with a framework shared-texture path and a protected comparison path.
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
        HRESULT reset(DWORD timeoutMs = 10000);
        HRESULT execute(const d3d::mgpu::SsaoConstants& constants, CrossAdapterSyncPoint normalsReady = {}, CrossAdapterSyncPoint depthReady = {}, uint64_t frame = 0, uint32_t resourceIndex = 0);
        // Copies shared inputs, computes AO, and publishes shared output in one direct list.
        // S_FALSE means all three command/constant recordings are busy; never waits or grows the budget.
        HRESULT executeFramework(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared, uint64_t frame = 0);
        HRESULT executeMailbox(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared,
            const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion);
        void retainMailboxResources(const eastl::shared_ptr<void>& resources);
        d3d::mgpu::SsaoGpuTiming pollTiming();
        // The output-copy dependency is applied only when this set is rendered again or reset.
        HRESULT releaseOutput(CrossAdapterSyncPoint released, uint32_t resourceIndex = 0);
        CrossAdapterSyncPoint getReadyPoint(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getNormals(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getDepth(uint32_t resourceIndex = 0) const;
        ID3D12Resource* getResult(uint32_t resourceIndex = 0) const;
        // Borrowed queue for native integration/profiling; lifetime follows this pass.
        ID3D12CommandQueue* getQueue() const;
        static size_t collectRetiredResources();

    private:
        class State;
        static State* s_retired;
        eastl::unique_ptr<State> m_state;
    };
}  // namespace drv3d_dx12
