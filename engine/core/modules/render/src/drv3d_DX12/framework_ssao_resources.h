// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include "device_ssao_pass.h"
#include "shared_ssao_texture.h"

namespace drv3d_dx12
{
    // Experimental framework transport: one physical row-major texture per payload, opened on both
    // adapters. There are deliberately no copy queues, shared fences, tickets or protected slots.
    class FrameworkSsaoResources
    {
        SharedSsaoTexture m_normals, m_depth, m_result;

    public:
        HRESULT init(ID3D12Device* primary, ID3D12Device* secondary, const DeviceSsaoPassDesc& desc)
        {
            if (!primary || !secondary || !desc.width || !desc.height) return E_INVALIDARG;
            const UINT width = desc.inputWidth ? desc.inputWidth : desc.width;
            const UINT height = desc.inputHeight ? desc.inputHeight : desc.height;
            const UINT normalGroup = desc.packedNormals ? 8u : 4u;
            const UINT normalWidth = desc.packedFrameworkNormals ? (width + normalGroup - 1) / normalGroup : 0u;
            HRESULT result = desc.reconstructNormals ? S_OK :
                m_normals.init(primary, secondary, width, height, desc.packedNormals ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_FLOAT,
                    normalWidth, DXGI_FORMAT_R32G32B32A32_UINT);
            const UINT packedWidth = desc.packedFrameworkDepth ? (width + (desc.linearDepth16 ? 7u : 3u)) / (desc.linearDepth16 ? 8u : 4u) : 0u;
            if (SUCCEEDED(result)) result = m_depth.init(primary, secondary, width, height,
                desc.linearDepth16 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT, packedWidth, DXGI_FORMAT_R32G32B32A32_UINT);
            if (SUCCEEDED(result)) result = m_result.init(primary, secondary, desc.width, desc.height, DXGI_FORMAT_R8_UNORM);
            return result;
        }

        FrameworkSsaoTextures primary() const { return {m_normals.textures[0].Get(), m_depth.textures[0].Get(), m_result.textures[0].Get(), {m_normals.heaps[0].Get(), m_depth.heaps[0].Get(), m_result.heaps[0].Get()}}; }
        FrameworkSsaoTextures secondary() const { return {m_normals.textures[1].Get(), m_depth.textures[1].Get(), m_result.textures[1].Get(), {m_normals.heaps[1].Get(), m_depth.heaps[1].Get(), m_result.heaps[1].Get()}}; }
    };
}
