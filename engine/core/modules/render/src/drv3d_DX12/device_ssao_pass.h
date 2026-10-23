// device_ssao_pass.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
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
        D3D12_SHADER_BYTECODE vertexShader = {};
        D3D12_SHADER_BYTECODE rawShader = {};
        D3D12_SHADER_BYTECODE blurShader = {};
        eastl::shared_ptr<d3d::mgpu::ISsaoProfiler> profiler;
    };

    /**
     * @brief Three AO draws on one explicit device/queue. Inputs and outputs are COMMON between submissions.
     * Channel endpoints and their fences stay fixed until reset. Input copies wait for getReadyPoint(); execute
     * waits for both copies. releaseOutput orders the next overwrite after the output channel has read it.
     * Command/constant slots are polled: busy never blocks the render thread. Destruction retires pending work.
     */
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
        HRESULT reset(DWORD timeoutMs = 10000);
        HRESULT canSubmit() const;
        HRESULT execute(const d3d::mgpu::SsaoConstants& constants, bool blur, CrossAdapterSyncPoint normalsReady = {}, CrossAdapterSyncPoint depthReady = {});
        HRESULT releaseOutput(CrossAdapterSyncPoint released, bool raw = false);
        CrossAdapterSyncPoint getReadyPoint() const;
        ID3D12Resource* getNormals() const;
        ID3D12Resource* getDepth() const;
        ID3D12Resource* getResult(bool raw = false) const;
        static size_t collectRetiredResources();

    private:
        class State;
        static State* s_retired;
        eastl::unique_ptr<State> m_state;
    };
}  // namespace drv3d_dx12
