// Copyright 2026 N-GINN LLC. All rights reserved.

#include "device_resource.h"

namespace drv3d_dx12
{
HRESULT DeviceResource::create(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value)
{
    if (!device)
    {
        return E_INVALIDARG;
    }
    if (resource)
    {
        return DXGI_ERROR_INVALID_CALL;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heap_type;
    // GPU 1 is a separate device, not node 1 of the primary device.
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    Microsoft::WRL::ComPtr<ID3D12Resource> created;
    const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, clear_value, IID_PPV_ARGS(&created));
    if (FAILED(result))
    {
        return result;
    }

    owner = device;
    resource.Swap(created);
    initialState = state;
    return S_OK;
}

HRESULT DeviceResource::createRenderTarget(ID3D12Device* device, UINT width, UINT height)
{
    if (width == 0 || height == 0 || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        return E_INVALIDARG;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = desc.Format;
    return create(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, &clear);
}

HRESULT DeviceResource::createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heap_type)
{
    if (size == 0)
    {
        return E_INVALIDARG;
    }
    D3D12_RESOURCE_STATES state;
    switch (heap_type)
    {
        case D3D12_HEAP_TYPE_DEFAULT: state = D3D12_RESOURCE_STATE_COMMON; break;
        case D3D12_HEAP_TYPE_UPLOAD: state = D3D12_RESOURCE_STATE_GENERIC_READ; break;
        case D3D12_HEAP_TYPE_READBACK: state = D3D12_RESOURCE_STATE_COPY_DEST; break;
        default: return E_INVALIDARG;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return create(device, desc, heap_type, state, nullptr);
}

void DeviceResource::reset()
{
    resource.Reset();
    owner.Reset();
    initialState = D3D12_RESOURCE_STATE_COMMON;
}
}
