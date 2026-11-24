// device_context.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/mgpu/bridge/device_context.h"

#include "nau/mgpu/Graphics/GCommandQueue.h"
#include "nau/mgpu/Graphics/GCrossAdapterResource.h"
#include "nau/mgpu/Graphics/GDeviceFactory.h"
#include "nau/mgpu/Graphics/GResource.h"

namespace nau::mgpu
{
    using PEPEngine::Graphics::GCrossAdapterResource;
    using PEPEngine::Graphics::GDevice;
    using PEPEngine::Graphics::GDeviceFactory;
    using PEPEngine::Graphics::GResource;

    void shutdownDevices()
    {
        GDeviceFactory::Shutdown();
    }

    struct DeviceContext::Impl
    {
        std::shared_ptr<GDevice> owner;
    };

    DeviceContext::DeviceContext() :
        m_impl(std::make_unique<Impl>())
    {
    }
    DeviceContext::~DeviceContext() = default;

    HRESULT DeviceContext::attach(ID3D12Device* device)
    {
        if (!device || m_impl->owner)
        {
            return E_INVALIDARG;
        }
        try
        {
            GDeviceFactory::SetDebugLayerEnabled(false);
            m_impl->owner = GDeviceFactory::AcquireDevice(device->GetAdapterLuid(), device);
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    HRESULT DeviceContext::selectSecondary(LUID primary, const LUID* requested, bool useWarp)
    {
        if (m_impl->owner)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        try
        {
            GDeviceFactory::SetDebugLayerEnabled(false);
            const auto same = [](LUID a, LUID b)
            {
                return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
            };
            if (requested)
            {
                if (same(primary, *requested))
                {
                    return E_INVALIDARG;
                }
                m_impl->owner = GDeviceFactory::AcquireDevice(*requested);
            }
            else
            {
                for (const auto& candidate : GDeviceFactory::GetAllDevices(useWarp))
                {
                    const auto info = candidate->GetDesc();
                    if (same(info.AdapterLuid, primary))
                    {
                        continue;
                    }
                    const bool software = (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                    if (software != useWarp)
                    {
                        continue;
                    }
                    D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
                    const bool uma = SUCCEEDED(candidate->GetDXDevice()->CheckFeatureSupport(
                                         D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture))) &&
                                     architecture.UMA;
                    if (!m_impl->owner || uma)
                    {
                        m_impl->owner = candidate;
                    }
                    if (uma)
                    {
                        break;
                    }
                }
            }
            return m_impl->owner ? S_OK : DXGI_ERROR_NOT_FOUND;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    ID3D12Device* DeviceContext::device() const
    {
        return m_impl->owner ? m_impl->owner->GetDXDevice().Get() : nullptr;
    }

    ID3D12CommandQueue* DeviceContext::graphicsQueue() const
    {
        return m_impl->owner ? m_impl->owner->GetCommandQueue()->GetD3D12CommandQueue().Get() : nullptr;
    }

    DXGI_ADAPTER_DESC1 DeviceContext::description() const
    {
        DXGI_ADAPTER_DESC1 result = {};
        if (m_impl->owner)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(GDeviceFactory::GetFactory()->EnumAdapterByLuid(device()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
            {
                adapter->GetDesc1(&result);
            }
        }
        return result;
    }

    struct Resource::Impl
    {
        std::unique_ptr<GResource> resource;
    };
    Resource::Resource() :
        m_impl(std::make_unique<Impl>())
    {
    }
    Resource::~Resource() = default;

    HRESULT Resource::createTexture(DeviceContext* context, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, UINT mipLevels)
    {
        if (!context || !context->device() || !width || !height || !mipLevels || m_impl->resource)
        {
            return E_INVALIDARG;
        }
        try
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, static_cast<UINT16>(mipLevels), 1, 0, flags);
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = format;
            m_impl->resource = std::make_unique<GResource>(context->m_impl->owner, desc, L"Effect texture",
                                                           (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clear : nullptr);
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    HRESULT Resource::createBuffer(DeviceContext* context, UINT64 bytes, D3D12_HEAP_TYPE heapType)
    {
        if (!context || !context->device() || !bytes || m_impl->resource ||
            (heapType != D3D12_HEAP_TYPE_DEFAULT && heapType != D3D12_HEAP_TYPE_UPLOAD && heapType != D3D12_HEAP_TYPE_READBACK))
        {
            return E_INVALIDARG;
        }
        try
        {
            const auto state = heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : heapType == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                                                                                             : D3D12_RESOURCE_STATE_COMMON;
            m_impl->resource = std::make_unique<GResource>(context->m_impl->owner, CD3DX12_RESOURCE_DESC::Buffer(bytes),
                                                           L"Effect buffer", nullptr, state, CD3DX12_HEAP_PROPERTIES(heapType));
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    ID3D12Resource* Resource::getResource() const
    {
        return m_impl->resource ? m_impl->resource->GetD3D12Resource().Get() : nullptr;
    }

    struct SharedResource::Impl
    {
        std::unique_ptr<GCrossAdapterResource> allocation;
    };
    SharedResource::SharedResource() :
        m_impl(std::make_unique<Impl>())
    {
    }
    SharedResource::~SharedResource() = default;

    HRESULT SharedResource::init(DeviceContext& primary, DeviceContext& secondary, const D3D12_RESOURCE_DESC& desc, const D3D12_RESOURCE_DESC* peer)
    {
        if (!primary.device() || !secondary.device() || m_impl->allocation)
        {
            return E_INVALIDARG;
        }
        try
        {
            m_impl->allocation = std::make_unique<GCrossAdapterResource>(desc, peer ? *peer : desc,
                                                                         primary.m_impl->owner, secondary.m_impl->owner);
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    ID3D12Resource* SharedResource::resource(UINT side) const
    {
        if (!m_impl->allocation || side > 1)
        {
            return nullptr;
        }
        return (side == 0 ? m_impl->allocation->GetPrimeResource() : m_impl->allocation->GetSharedResource()).GetD3D12Resource().Get();
    }

    ID3D12Heap* SharedResource::heap(UINT side) const
    {
        return m_impl->allocation ? m_impl->allocation->GetHeap(side) : nullptr;
    }

    void SharedResource::setName(const wchar_t* name) const
    {
        for (UINT side = 0; side < 2; ++side)
        {
            if (auto* value = resource(side))
            {
                value->SetName(name);
            }
        }
    }
}  // namespace nau::mgpu
