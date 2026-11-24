// multi_gpu_device.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
//
#pragma once

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>

#include "multi_gpu_status.h"
#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    inline bool is_software_device(const DXGI_ADAPTER_DESC1& desc)
    {
        constexpr UINT software_driver_vendor = 0x1414;
        constexpr UINT software_driver_id = 0x8c;
        // Some runtimes omit the software flag, so also check the WARP vendor and device ids.
        return (0 != (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) ||
               (desc.VendorId == software_driver_vendor && desc.DeviceId == software_driver_id);
    }

    inline bool is_secondary_gpu_candidate(const DXGI_ADAPTER_DESC1& candidate, LUID primary_luid)
    {
        const bool sameAdapter =
            candidate.AdapterLuid.LowPart == primary_luid.LowPart && candidate.AdapterLuid.HighPart == primary_luid.HighPart;
        return !is_software_device(candidate) && !sameAdapter;
    }

    struct CrossAdapterSupport
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        HRESULT queryResult = E_FAIL;

        void query(ID3D12Device* device)
        {
            options = {};
            queryResult = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
            if (FAILED(queryResult))
            {
                options = {};
            }
        }
    };

}  // namespace drv3d_dx12
