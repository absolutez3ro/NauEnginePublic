// Copyright 2024 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include "nau/directx/d3d12.h"
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace drv3d_dx12
{
inline bool is_software_device(const DXGI_ADAPTER_DESC1 &desc)
{
  constexpr UINT software_driver_vendor = 0x1414;
  constexpr UINT software_driver_id = 0x8c;
  // Some runtimes omit the software flag, so also check the WARP vendor and device ids.
  return (0 != (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) ||
         (desc.VendorId == software_driver_vendor && desc.DeviceId == software_driver_id);
}

inline bool is_secondary_gpu_candidate(const DXGI_ADAPTER_DESC1 &candidate, LUID primary_luid)
{
  const bool sameAdapter = candidate.AdapterLuid.LowPart == primary_luid.LowPart &&
    candidate.AdapterLuid.HighPart == primary_luid.HighPart;
  return !is_software_device(candidate) && !sameAdapter;
}

struct CrossAdapterSupport
{
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  HRESULT queryResult = E_FAIL;

  void query(ID3D12Device *device)
  {
    options = {};
    queryResult = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(queryResult))
      options = {};
  }
};

class SecondaryGpuDevice
{
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  DXGI_ADAPTER_DESC1 description = {};
  CrossAdapterSupport crossAdapterSupport;

public:
  SecondaryGpuDevice() = default;
  SecondaryGpuDevice(const SecondaryGpuDevice &) = delete;
  SecondaryGpuDevice &operator=(const SecondaryGpuDevice &) = delete;

  HRESULT init(IDXGIAdapter1 *candidate, LUID primary_luid, D3D_FEATURE_LEVEL feature_level,
    PFN_D3D12_CREATE_DEVICE create_device)
  {
    reset();
    if (!candidate || !create_device)
      return E_INVALIDARG;
    DXGI_ADAPTER_DESC1 info = {};
    HRESULT hr = candidate->GetDesc1(&info);
    if (FAILED(hr))
      return hr;
    if (!is_secondary_gpu_candidate(info, primary_luid))
      return E_INVALIDARG;

    Microsoft::WRL::ComPtr<ID3D12Device> newDevice;
    hr = create_device(candidate, feature_level, IID_PPV_ARGS(&newDevice));
    if (FAILED(hr))
      return hr;
    if (!newDevice)
      return E_FAIL;

    adapter = candidate;
    description = info;
    device = newDevice;
    crossAdapterSupport.query(device.Get());
    return S_OK;
  }

  void reset()
  {
    device.Reset();
    adapter.Reset();
    description = {};
    crossAdapterSupport = {};
  }

  ID3D12Device *getDevice() const { return device.Get(); }
  const DXGI_ADAPTER_DESC1 &getDescription() const { return description; }
  const CrossAdapterSupport &getCrossAdapterSupport() const { return crossAdapterSupport; }
};
}
