// ao_device_pass.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <EASTL/shared_ptr.h>
#include <EASTL/unique_ptr.h>

#include "nau/3d/ao_constants.h"
#include "nau/3d/ao_executor.h"
#include "nau/mgpu/bridge/device_context.h"
#include "nau/mgpu/mailbox/mailbox_job.h"

namespace drv3d_dx12
{
    struct AoDevicePassDesc
    {
        std::shared_ptr<nau::mgpu::DeviceContext> context;
        UINT width = 0;
        UINT height = 0;
        UINT inputWidth = 0;
        UINT inputHeight = 0;
        D3D12_SHADER_BYTECODE vertexShader = {};
        D3D12_SHADER_BYTECODE rawShader = {};
        D3D12_SHADER_BYTECODE rawComputeShader = {};
        nau::render::ao::NativeAlgorithmFactory createAlgorithm = nullptr;
        const nau::render::ao::ShaderBytecode* algorithmShaders = nullptr;
        uint32_t algorithmShaderCount = 0;
        bool reconstructNormals = false;
        bool packedNormals = false;
        bool linearDepth16 = false;
        bool measureTimings = false;
        bool packedFrameworkDepth = false;
        bool packedFrameworkNormals = false;
    };

    /** @brief Shared textures as seen by GPU1. */
    struct AoDeviceInputs
    {
        ID3D12Resource* normals = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* result = nullptr;
    };

    /** @brief Native GPU1 AO compute and publication for mailbox 2b. */
    class AoDevicePass final
    {
    public:
        AoDevicePass();
        AoDevicePass(const AoDevicePass&) = delete;
        AoDevicePass(AoDevicePass&&) = delete;
        AoDevicePass& operator=(const AoDevicePass&) = delete;
        AoDevicePass& operator=(AoDevicePass&&) = delete;
        ~AoDevicePass();

        HRESULT init(const AoDevicePassDesc& desc);
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
        HRESULT executeMailbox(const nau::render::AoConstants& constants, const AoDeviceInputs& shared, const nau::mgpu::MailboxJob& job, uint64_t frame, nau::mgpu::MailboxCompletion& completion);
        void retainMailboxResources(const eastl::shared_ptr<void>& resources);
        nau::render::ao::ExecutionStats pollTiming();
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
