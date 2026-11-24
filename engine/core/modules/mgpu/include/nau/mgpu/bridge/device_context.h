// device_context.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <memory>

namespace nau::mgpu
{
    /** @brief Drains common queues after all effects have been destroyed. */
    void shutdownDevices();
    /** @brief Retains a device and its queues from the common MGPU device factory. */
    class DeviceContext
    {
    public:
        DeviceContext();
        ~DeviceContext();
        DeviceContext(const DeviceContext&) = delete;
        DeviceContext& operator=(const DeviceContext&) = delete;

        /** @brief Adopts Nau's device without creating another device on that adapter. */
        HRESULT attach(ID3D12Device* device);
        /** @brief Selects a distinct adapter, preferring UMA, or the exact requested LUID. */
        HRESULT selectSecondary(LUID primary, const LUID* requested = nullptr, bool useWarp = false);
#if defined(NAU_ENABLE_MGPU)
        ID3D12Device* device() const;
#else
        ID3D12Device* device() const
        {
            return nullptr;
        }
#endif
        ID3D12CommandQueue* graphicsQueue() const;
        DXGI_ADAPTER_DESC1 description() const;

    private:
        friend class Resource;
        friend class SharedResource;
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    /** @brief A GResource allocation. The caller retires GPU uses before destruction. */
    class Resource
    {
    public:
        Resource();
        ~Resource();
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;
        HRESULT createTexture(DeviceContext* context, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, UINT mipLevels = 1);
        HRESULT createBuffer(DeviceContext* context, UINT64 bytes, D3D12_HEAP_TYPE heapType);
        ID3D12Resource* getResource() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    /** @brief One GCrossAdapterResource allocation, opened on both devices. */
    class SharedResource
    {
    public:
        SharedResource();
        ~SharedResource();
        SharedResource(const SharedResource&) = delete;
        SharedResource& operator=(const SharedResource&) = delete;
        HRESULT init(DeviceContext& primary, DeviceContext& secondary, const D3D12_RESOURCE_DESC& description, const D3D12_RESOURCE_DESC* peer = nullptr);
        ID3D12Resource* resource(UINT side) const;
        ID3D12Heap* heap(UINT side) const;
        void setName(const wchar_t* name) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}  // namespace nau::mgpu
