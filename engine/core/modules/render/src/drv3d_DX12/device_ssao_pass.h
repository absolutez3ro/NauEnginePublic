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
        D3D12_SHADER_BYTECODE vertexShader = {};
        D3D12_SHADER_BYTECODE rawShader = {};
    };

    /**
     * @brief Raw half-resolution SSAO on GPU 1. Queue waits order input copies and output reuse.
     * Inputs and output are COMMON between submissions. Command/constant storage grows when all
     * recordings are in flight; resource recycling never skips a frame or waits on the CPU.
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
        HRESULT execute(const d3d::mgpu::SsaoConstants& constants, CrossAdapterSyncPoint normalsReady = {}, CrossAdapterSyncPoint depthReady = {});
        HRESULT releaseOutput(CrossAdapterSyncPoint released);
        CrossAdapterSyncPoint getReadyPoint() const;
        ID3D12Resource* getNormals() const;
        ID3D12Resource* getDepth() const;
        ID3D12Resource* getResult() const;
        static size_t collectRetiredResources();

    private:
        class State;
        static State* s_retired;
        eastl::unique_ptr<State> m_state;
    };
}  // namespace drv3d_dx12
