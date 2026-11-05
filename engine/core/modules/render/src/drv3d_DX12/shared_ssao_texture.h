// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <wrl/client.h>

#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    // A single allocation opened on both adapters. Texture states are owned by each caller's queue.
    struct SharedSsaoTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> heaps[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> textures[2];

        HRESULT init(ID3D12Device* primary, ID3D12Device* secondary, UINT width, UINT height, DXGI_FORMAT format,
                     UINT peerWidth = 0, DXGI_FORMAT peerFormat = DXGI_FORMAT_UNKNOWN)
        {
            if (!primary || !secondary || !width || !height || textures[0] || textures[1]) return E_INVALIDARG;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Format = format;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            auto peer = desc;
            if (peerWidth)
            {
                peer.Width = peerWidth;
                peer.Format = peerFormat;
                // The wide software view must address exactly the same rows of the shared allocation.
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT sourceLayout = {}, peerLayout = {};
                UINT sourceRows = 0, peerRows = 0;
                primary->GetCopyableFootprints(&desc, 0, 1, 0, &sourceLayout, &sourceRows, nullptr, nullptr);
                secondary->GetCopyableFootprints(&peer, 0, 1, 0, &peerLayout, &peerRows, nullptr, nullptr);
                if (sourceLayout.Offset != peerLayout.Offset || sourceRows != peerRows ||
                    sourceLayout.Footprint.RowPitch != peerLayout.Footprint.RowPitch) return DXGI_ERROR_UNSUPPORTED;
            }
            const auto a = primary->GetResourceAllocationInfo(0, 1, &desc);
            const auto b = secondary->GetResourceAllocationInfo(0, 1, &peer);
            if (a.SizeInBytes == UINT64_MAX || b.SizeInBytes == UINT64_MAX || !a.SizeInBytes || !b.SizeInBytes)
                return DXGI_ERROR_UNSUPPORTED;
            D3D12_HEAP_DESC heap = {};
            heap.SizeInBytes = a.SizeInBytes > b.SizeInBytes ? a.SizeInBytes : b.SizeInBytes;
            heap.Alignment = a.Alignment > b.Alignment ? a.Alignment : b.Alignment;
            heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
            HRESULT result = primary->CreateHeap(&heap, IID_PPV_ARGS(&heaps[0]));
            HANDLE handle = nullptr;
            if (SUCCEEDED(result)) result = primary->CreateSharedHandle(heaps[0].Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            if (SUCCEEDED(result))
            {
                result = secondary->OpenSharedHandle(handle, IID_PPV_ARGS(&heaps[1]));
                CloseHandle(handle);
            }
            if (SUCCEEDED(result)) result = primary->CreatePlacedResource(heaps[0].Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures[0]));
            if (SUCCEEDED(result)) result = secondary->CreatePlacedResource(heaps[1].Get(), 0, &peer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures[1]));
            return result;
        }

        void setName(const wchar_t* name) const
        {
            for (const auto& texture : textures)
                if (texture) texture->SetName(name);
        }
    };
}
