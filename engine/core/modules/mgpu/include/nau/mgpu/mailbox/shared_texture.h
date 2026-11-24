// shared_texture.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <wrl/client.h>

#include "nau/mgpu/bridge/device_context.h"

namespace nau::mgpu
{
    /** @brief One shared allocation opened on both adapters. Each caller manages its own texture state. */
    struct SharedTexture
    {
        SharedResource allocation;

        HRESULT init(DeviceContext& primaryContext, DeviceContext& secondaryContext, UINT width, UINT height, DXGI_FORMAT format, UINT peerWidth = 0, DXGI_FORMAT peerFormat = DXGI_FORMAT_UNKNOWN)
        {
            auto* primary = primaryContext.device();
            auto* secondary = secondaryContext.device();
            if (!primary || !secondary || !width || !height || allocation.resource(0))
            {
                return E_INVALIDARG;
            }
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
                    sourceLayout.Footprint.RowPitch != peerLayout.Footprint.RowPitch)
                {
                    return DXGI_ERROR_UNSUPPORTED;
                }
            }
            return allocation.init(primaryContext, secondaryContext, desc, &peer);
        }

        ID3D12Resource* resource(UINT side) const
        {
            return allocation.resource(side);
        }

        void setName(const wchar_t* name) const
        {
            allocation.setName(name);
        }
    };
}  // namespace nau::mgpu
