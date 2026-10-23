// Copyright 2026 N-GINN LLC. All rights reserved.

#include "nau_device_resource_allocator.h"

#include "device.h"

namespace drv3d_dx12
{
    namespace
    {
        class SecondaryHeapPool final : public resource_manager::ResourceMemoryHeapProvider
        {
        public:
            SecondaryHeapPool() = default;
            SecondaryHeapPool(const SecondaryHeapPool&) = delete;
            SecondaryHeapPool(SecondaryHeapPool&&) = delete;
            SecondaryHeapPool& operator=(const SecondaryHeapPool&) = delete;
            SecondaryHeapPool& operator=(SecondaryHeapPool&&) = delete;
            ~SecondaryHeapPool() = default;

            using resource_manager::ResourceMemoryHeapProvider::setup;
            using resource_manager::ResourceMemoryHeapProvider::SetupInfo;
            using resource_manager::ResourceMemoryHeapProvider::shutdown;

            size_t getHeapCount() const
            {
                size_t count = 0;
                for (const eastl::vector<ResourceHeap>& group : groups)
                {
                    for (const ResourceHeap& heap : group)
                    {
                        count += static_cast<bool>(heap) ? 1 : 0;
                    }
                }
                return count;
            }
        };

        DeviceMemoryClass getMemoryClass(const DeviceResourceRequest& request)
        {
            switch (request.heapType)
            {
                case D3D12_HEAP_TYPE_DEFAULT:
                    return request.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                               ? DeviceMemoryClass::DEVICE_RESIDENT_BUFFER
                               : DeviceMemoryClass::DEVICE_RESIDENT_IMAGE;
                case D3D12_HEAP_TYPE_UPLOAD:
                    return DeviceMemoryClass::PUSH_RING_BUFFER;
                case D3D12_HEAP_TYPE_READBACK:
                    return DeviceMemoryClass::READ_BACK_BUFFER;
                default:
                    return DeviceMemoryClass::INVALID;
            }
        }
    }  // namespace

    class NauDeviceResourceAllocator::State
    {
    public:
        struct Allocation
        {
            uint64_t token;
            ResourceMemory memory;
        };

        State() = default;
        State(const State&) = delete;
        State(State&&) = delete;
        State& operator=(const State&) = delete;
        State& operator=(State&&) = delete;

        ~State()
        {
            NAU_ASSERT(m_allocations.empty(), "Secondary GPU resources must be destroyed before their allocator");
            m_pool.shutdown();
        }

        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        Microsoft::WRL::ComPtr<DXGIAdapter> m_adapter;
        SecondaryHeapPool m_pool;
        eastl::vector<Allocation> m_allocations;
        uint64_t m_nextToken = 1;
    };

    NauDeviceResourceAllocator::NauDeviceResourceAllocator() = default;
    NauDeviceResourceAllocator::~NauDeviceResourceAllocator() = default;

    HRESULT NauDeviceResourceAllocator::init(ID3D12Device* device)
    {
        if (!device)
        {
            return E_INVALIDARG;
        }
        if (m_state)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        Microsoft::WRL::ComPtr<DXGIAdapter> adapter;
        if (SUCCEEDED(result))
        {
            result = factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
        }
        if (FAILED(result))
        {
            return result;
        }
        m_state = eastl::make_unique<State>();
        m_state->m_device = device;
        m_state->m_adapter = adapter;
        SecondaryHeapPool::SetupInfo setup = {};
        setup.device = device;
        setup.adapter = adapter.Get();
        setup.reportErrorsToDriver = false;
        setup.useLocalBudgetForUMA = true;
        m_state->m_pool.setup(setup);
        return S_OK;
    }

    HRESULT NauDeviceResourceAllocator::shutdown()
    {
        if (m_state && !m_state->m_allocations.empty())
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        m_state.reset();
        return S_OK;
    }

    ID3D12Device* NauDeviceResourceAllocator::getDevice() const
    {
        return m_state ? m_state->m_device.Get() : nullptr;
    }

    HRESULT NauDeviceResourceAllocator::allocate(const DeviceResourceRequest& request,
                                                 Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                                                 uint64_t& token)
    {
        if (!m_state || resource || token != 0)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        const DeviceMemoryClass memoryClass = getMemoryClass(request);
        if (memoryClass == DeviceMemoryClass::INVALID ||
            (request.heapType != D3D12_HEAP_TYPE_DEFAULT && request.description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER))
        {
            return E_INVALIDARG;
        }
        const D3D12_RESOURCE_ALLOCATION_INFO info = m_state->m_device->GetResourceAllocationInfo(0, 1, &request.description);
        if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes == 0 || info.Alignment == 0 || m_state->m_nextToken == UINT64_MAX)
        {
            return E_INVALIDARG;
        }
        const auto properties = m_state->m_pool.getProperties(request.description.Flags, memoryClass, info.Alignment);
        HRESULT allocationError = E_OUTOFMEMORY;
        const ResourceMemory memory = m_state->m_pool.allocate(m_state->m_adapter.Get(), m_state->m_device.Get(), properties, info, {}, &allocationError);
        if (!memory)
        {
            return FAILED(allocationError) ? allocationError : E_OUTOFMEMORY;
        }
        const HRESULT result = m_state->m_device->CreatePlacedResource(memory.getHeap(), memory.getOffset(),
                                                                       &request.description, request.initialState, request.clearValue, IID_PPV_ARGS(&resource));
        if (FAILED(result))
        {
            m_state->m_pool.free(memory);
            return result;
        }
        token = m_state->m_nextToken++;
        m_state->m_allocations.push_back({token, memory});
        return S_OK;
    }

    void NauDeviceResourceAllocator::releaseAllocation(uint64_t token)
    {
        if (!m_state)
        {
            return;
        }
        for (auto allocation = m_state->m_allocations.begin(); allocation != m_state->m_allocations.end(); ++allocation)
        {
            if (allocation->token == token)
            {
                m_state->m_pool.free(allocation->memory);
                m_state->m_allocations.erase(allocation);
                return;
            }
        }
        NAU_ASSERT(false, "Unknown secondary GPU allocation token");
    }

    size_t NauDeviceResourceAllocator::getAllocationCount() const
    {
        return m_state ? m_state->m_allocations.size() : 0;
    }

    size_t NauDeviceResourceAllocator::getHeapCount() const
    {
        return m_state ? m_state->m_pool.getHeapCount() : 0;
    }
}  // namespace drv3d_dx12
