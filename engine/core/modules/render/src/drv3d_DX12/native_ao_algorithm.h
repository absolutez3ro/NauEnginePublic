// native_ao_algorithm.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once
#include "nau/3d/ao_executor.h"
#include "nau/mgpu/bridge/device_context.h"
namespace nau::render::ao
{
    /** @brief Algorithm-owned native passes. Scheduling and transport belong to the executor. */
    class NativeAlgorithm
    {
    public:
        virtual ~NativeAlgorithm() = default;
        virtual HRESULT initialize(const std::shared_ptr<mgpu::DeviceContext>& context, UINT width, UINT height, ID3D12Resource* normals, ID3D12Resource* depth, ID3D12Resource* random, ID3D12Resource* output, const ShaderBytecode* shaders, uint32_t shaderCount) = 0;
        /** @brief Records an algorithm with inputs in NON_PIXEL_SHADER_RESOURCE and output in COMMON. */
        virtual void record(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS constants) = 0;
    };
}  // namespace nau::render::ao
