// test_resource_allocator.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// Standalone tests isolate the resource contract; engine builds exercise the real Nau pool.
#pragma once

#include <EASTL/allocator_malloc.h>
#include <EASTL/shared_ptr.h>

#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    #include "nau_device_resource_allocator.h"

namespace mgpu_test
{
    using TestResourceAllocator = drv3d_dx12::NauDeviceResourceAllocator;
}
#else
    #include "device_resource_allocator.h"

namespace mgpu_test
{
    /**
     * @brief Test-only allocator for builds without Render/NauKernel. Never used by production code.
     */
    class TestResourceAllocator final : public drv3d_dx12::IDeviceResourceAllocator
    {
    public:
        TestResourceAllocator() = default;
        TestResourceAllocator(const TestResourceAllocator&) = delete;
        TestResourceAllocator(TestResourceAllocator&&) = delete;
        TestResourceAllocator& operator=(const TestResourceAllocator&) = delete;
        TestResourceAllocator& operator=(TestResourceAllocator&&) = delete;
        ~TestResourceAllocator() override = default;

        HRESULT init(ID3D12Device* device)
        {
            if (!device)
            {
                return E_INVALIDARG;
            }
            if (m_device)
            {
                return DXGI_ERROR_INVALID_CALL;
            }
            m_device = device;
            return S_OK;
        }

        HRESULT shutdown()
        {
            if (m_liveCount != 0)
            {
                return DXGI_ERROR_INVALID_CALL;
            }
            m_device.Reset();
            return S_OK;
        }

        ID3D12Device* getDevice() const override
        {
            return m_device.Get();
        }

        HRESULT allocate(const drv3d_dx12::DeviceResourceRequest& request,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                         uint64_t& token) override
        {
            if (!m_device || resource || token != 0)
            {
                return DXGI_ERROR_INVALID_CALL;
            }
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = request.heapType;
            heap.CreationNodeMask = 1;
            heap.VisibleNodeMask = 1;
            const HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                                     &request.description, request.initialState, request.clearValue, IID_PPV_ARGS(&resource));
            if (SUCCEEDED(result))
            {
                token = m_nextToken++;
                ++m_liveCount;
            }
            return result;
        }

        void releaseAllocation(uint64_t) override
        {
            --m_liveCount;
        }

        size_t getAllocationCount() const
        {
            return m_liveCount;
        }

    private:
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        uint64_t m_nextToken = 1;
        size_t m_liveCount = 0;
    };
}  // namespace mgpu_test
#endif

namespace mgpu_test
{
    /** @brief Shared lifetime for pass retirement; malloc-backed control block also works without NauKernel. */
    inline eastl::shared_ptr<TestResourceAllocator> makeTestResourceAllocator()
    {
        return eastl::allocate_shared<TestResourceAllocator>(eastl::allocator_malloc{});
    }
}  // namespace mgpu_test
