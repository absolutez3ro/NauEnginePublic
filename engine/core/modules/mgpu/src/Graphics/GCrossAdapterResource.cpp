#include "nau/mgpu/Graphics/GCrossAdapterResource.h"

#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GResource.h"


namespace nau::PEPEngine::Graphics
{
    bool GCrossAdapterResource::IsInit() const
    {
        return isInit;
    }

    GCrossAdapterResource::GCrossAdapterResource(D3D12_RESOURCE_DESC& desc, const std::shared_ptr<GDevice>& primeDevice,
                                                 const std::shared_ptr<GDevice>& sharedDevice, const std::wstring& name,
                                                 const D3D12_RESOURCE_STATES initialState,
                                                 const D3D12_CLEAR_VALUE* clearValue)
        : GCrossAdapterResource(desc, desc, primeDevice, sharedDevice, name, initialState, clearValue)
    {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    }

    GCrossAdapterResource::GCrossAdapterResource(const D3D12_RESOURCE_DESC& primaryDesc,
        const D3D12_RESOURCE_DESC& secondaryDesc, const std::shared_ptr<GDevice>& primary,
        const std::shared_ptr<GDevice>& secondary, const std::wstring& name,
        D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue)
    {
        auto first = primaryDesc;
        auto second = secondaryDesc;
        first.Flags = second.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        first.Layout = second.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto a = primary->GetDXDevice()->GetResourceAllocationInfo(0, 1, &first);
        const auto b = secondary->GetDXDevice()->GetResourceAllocationInfo(0, 1, &second);
        if (!a.SizeInBytes || !b.SizeInBytes || a.SizeInBytes == UINT64_MAX || b.SizeInBytes == UINT64_MAX)
        {
            ThrowIfFailed(DXGI_ERROR_UNSUPPORTED);
        }
        D3D12_HEAP_DESC heap = {};
        heap.SizeInBytes = std::max(a.SizeInBytes, b.SizeInBytes);
        heap.Alignment = std::max(a.Alignment, b.Alignment);
        heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        ThrowIfFailed(primary->GetDXDevice()->CreateHeap(&heap, IID_PPV_ARGS(&crossAdapterResourceHeap[0])));
        HANDLE handle = nullptr;
        ThrowIfFailed(primary->GetDXDevice()->CreateSharedHandle(crossAdapterResourceHeap[0].Get(), nullptr,
            GENERIC_ALL, nullptr, &handle));
        const HRESULT result = secondary->GetDXDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&crossAdapterResourceHeap[1]));
        CloseHandle(handle);
        ThrowIfFailed(result);
        primeResource = std::make_shared<GResource>(primary, first, crossAdapterResourceHeap[0], name, clearValue, initialState);
        sharedResource = std::make_shared<GResource>(secondary, second, crossAdapterResourceHeap[1], name + L" Shared");
        isInit = true;
    }

    const GResource& GCrossAdapterResource::GetPrimeResource() const
    {
        return *primeResource.get();
    }

    const GResource& GCrossAdapterResource::GetSharedResource() const
    {
        return *sharedResource.get();
    }

    void GCrossAdapterResource::Reset()
    {
        if (!isInit) return;

        if (primeResource)
        {
            primeResource->Reset();
            primeResource.reset();
        }

        if (sharedResource)
        {
            sharedResource->Reset();
            sharedResource.reset();
        }

        isInit = false;
    }

    void GCrossAdapterResource::Resize(const UINT newWidth, const UINT newHeight)
    {
        auto desc = primeResource->GetD3D12ResourceDesc();
        desc.Width = newWidth;
        desc.Height = newHeight;


        primeResource = std::make_shared<GResource>(primeResource->GetDevice(), desc, crossAdapterResourceHeap[0],
                                                    primeResource->GetName());

        sharedResource = std::make_shared<GResource>(sharedResource->GetDevice(), desc, crossAdapterResourceHeap[1],
                                                     sharedResource->GetName());
    }
}
