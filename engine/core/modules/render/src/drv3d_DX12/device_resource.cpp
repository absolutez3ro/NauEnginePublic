// Copyright 2026 N-GINN LLC. All rights reserved.

#include "device_resource.h"

namespace drv3d_dx12
{
    DeviceResource::~DeviceResource()
    {
        reset();
    }

    HRESULT DeviceResource::create(IDeviceResourceAllocator* allocator, const DeviceResourceRequest& request)
    {
        if (!allocator || !allocator->getDevice())
        {
            return E_INVALIDARG;
        }
        if (m_resource)
        {
            return DXGI_ERROR_INVALID_CALL;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> created;
        uint64_t token = 0;
        const HRESULT result = allocator->allocate(request, created, token);
        if (FAILED(result))
        {
            return result;
        }

        m_owner = allocator->getDevice();
        m_resource.Swap(created);
        m_initialState = request.initialState;
        m_allocator = allocator;
        m_allocationToken = token;
        return S_OK;
    }

    HRESULT DeviceResource::createRenderTarget(IDeviceResourceAllocator* allocator, UINT width, UINT height)
    {
        return createTexture(allocator, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }

    HRESULT DeviceResource::createTexture(IDeviceResourceAllocator* allocator, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
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
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        return create(allocator, {desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                  (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 ? &clear : nullptr});
    }

    HRESULT DeviceResource::createBuffer(IDeviceResourceAllocator* allocator, UINT64 size, D3D12_HEAP_TYPE heapType)
    {
        if (size == 0)
        {
            return E_INVALIDARG;
        }
        D3D12_RESOURCE_STATES state;
        switch (heapType)
        {
            case D3D12_HEAP_TYPE_DEFAULT:
                state = D3D12_RESOURCE_STATE_COMMON;
                break;
            case D3D12_HEAP_TYPE_UPLOAD:
                state = D3D12_RESOURCE_STATE_GENERIC_READ;
                break;
            case D3D12_HEAP_TYPE_READBACK:
                state = D3D12_RESOURCE_STATE_COPY_DEST;
                break;
            default:
                return E_INVALIDARG;
        }

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return create(allocator, {desc, heapType, state, nullptr});
    }

    void DeviceResource::reset()
    {
        m_resource.Reset();
        if (m_allocator)
        {
            m_allocator->releaseAllocation(m_allocationToken);
            m_allocator = nullptr;
            m_allocationToken = 0;
        }
        m_owner.Reset();
        m_initialState = D3D12_RESOURCE_STATE_COMMON;
    }

    ID3D12Device* DeviceResource::getDevice() const
    {
        return m_owner.Get();
    }

    ID3D12Resource* DeviceResource::getResource() const
    {
        return m_resource.Get();
    }

    D3D12_RESOURCE_STATES DeviceResource::getInitialState() const
    {
        return m_initialState;
    }
}  // namespace drv3d_dx12
