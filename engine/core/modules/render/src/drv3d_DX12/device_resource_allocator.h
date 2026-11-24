// device_resource_allocator.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// Device-local resource allocation contract for the secondary render path.
#pragma once

#include <wrl/client.h>

#include <cstdint>

#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    /**
     * @brief Describes one allocation request without choosing a heap allocation policy.
     */
    struct DeviceResourceRequest
    {
        D3D12_RESOURCE_DESC description = {};
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        const D3D12_CLEAR_VALUE* clearValue = nullptr;
    };

    /**
     * @brief Allocates resources on one device and returns their memory after GPU completion.
     *
     * The allocator must outlive every DeviceResource using it. DeviceSsaoPass retains shared
     * ownership, including after retirement, until its GPU work completes or the device is removed.
     * Calls are serialized by the owning render thread. A successful allocation returns a nonzero
     * token; releaseAllocation() is called after releasing the resource and completing GPU access.
     * The engine implementation uses Nau's ResourceMemoryHeapProvider.
     */
    class IDeviceResourceAllocator
    {
    public:
        IDeviceResourceAllocator() = default;
        IDeviceResourceAllocator(const IDeviceResourceAllocator&) = delete;
        IDeviceResourceAllocator(IDeviceResourceAllocator&&) = delete;
        IDeviceResourceAllocator& operator=(const IDeviceResourceAllocator&) = delete;
        IDeviceResourceAllocator& operator=(IDeviceResourceAllocator&&) = delete;
        virtual ~IDeviceResourceAllocator() = default;

        /** @brief Returns the borrowed device used by every allocation. */
        virtual ID3D12Device* getDevice() const = 0;
        /**
         * @brief Creates a resource on the allocator's device.
         * @param request Resource description and initial state.
         * @param[out] resource Native resource. Empty on failure.
         * @param[out] token Allocation identifier. Zero on failure.
         * @return HRESULT of the allocation.
         */
        virtual HRESULT allocate(const DeviceResourceRequest& request,
                                 Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                                 uint64_t& token) = 0;
        /**
         * @brief Releases memory after the native resource and its GPU uses have ended.
         * @param token Identifier returned by allocate().
         */
        virtual void releaseAllocation(uint64_t token) = 0;
    };
}  // namespace drv3d_dx12
