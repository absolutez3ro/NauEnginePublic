// nau_device_resource_allocator.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// Secondary-device resources backed by the existing Nau heap manager.
#pragma once

#include <EASTL/unique_ptr.h>

#include "device_resource_allocator.h"

namespace drv3d_dx12
{
    /**
     * @brief Reuses Nau heap pooling and memory-class policies for an explicit D3D12 device.
     *
     * This allocator owns a separate ResourceMemoryHeapProvider instance. It never allocates from
     * GPU 0's manager, never silently creates committed resources and does not share heaps between
     * adapters. All allocated resources must be destroyed after their fences complete and before
     * destroying this allocator. Call shutdown() to check that no live allocations remain.
     */
    class NauDeviceResourceAllocator final : public IDeviceResourceAllocator
    {
    public:
        NauDeviceResourceAllocator();
        NauDeviceResourceAllocator(const NauDeviceResourceAllocator&) = delete;
        NauDeviceResourceAllocator(NauDeviceResourceAllocator&&) = delete;
        NauDeviceResourceAllocator& operator=(const NauDeviceResourceAllocator&) = delete;
        NauDeviceResourceAllocator& operator=(NauDeviceResourceAllocator&&) = delete;
        ~NauDeviceResourceAllocator() override;

        /** @brief Resolves the device's adapter and initializes Nau's memory and descriptor pools. */
        HRESULT init(ID3D12Device* device);
        /** @brief Returns DXGI_ERROR_INVALID_CALL and retains memory while any allocation is alive. */
        HRESULT shutdown();
        ID3D12Device* getDevice() const override;
        HRESULT allocate(const DeviceResourceRequest& request,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                         uint64_t& token) override;
        void releaseAllocation(uint64_t token) override;
        /** @brief Returns the count of live allocations, useful for lifecycle diagnostics. */
        size_t getAllocationCount() const;
        /** @brief Returns the number of native heaps currently owned by the Nau pool. */
        size_t getHeapCount() const;

    private:
        class State;
        eastl::unique_ptr<State> m_state;
    };
}  // namespace drv3d_dx12
