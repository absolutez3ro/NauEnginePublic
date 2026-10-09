// device_resource.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include "nau/directx/d3d12.h"
#include <wrl/client.h>

namespace drv3d_dx12
{
// Native resources for an explicitly selected device, outside the primary resource manager.
// Borrowed getters must only be used with queues/descriptors from getDevice(). This class
// does not submit commands or track transitions. The caller must finish GPU work before
// reset/destruction, and release resources before driver shutdown or device recovery.
class DeviceResource
{
public:
    DeviceResource() = default;
    DeviceResource(const DeviceResource&) = delete;
    DeviceResource& operator=(const DeviceResource&) = delete;

    // Creation requires an empty object. Failure leaves it unchanged; nullptr never falls
    // back to the primary device. Call reset() explicitly before reusing an existing object.
    HRESULT createRenderTarget(ID3D12Device* device, UINT width, UINT height);
    HRESULT createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heap_type);
    void reset();

    ID3D12Device* getDevice() const { return owner.Get(); }
    ID3D12Resource* getResource() const { return resource.Get(); }
    // This is the creation state, not a tracker of subsequent command-list transitions.
    D3D12_RESOURCE_STATES getInitialState() const { return initialState; }

private:
    HRESULT create(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap_type,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value);

    // Reverse destruction order releases the resource before its explicit device reference.
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
};
}
