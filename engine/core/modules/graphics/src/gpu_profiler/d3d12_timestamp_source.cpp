#include "d3d12_timestamp_source.h"

#include <EASTL/algorithm.h>
#include <EASTL/sort.h>

#include "nau/diag/logging.h"

namespace nau::gpu_profiler
{
    D3D12TimestampSource::~D3D12TimestampSource()
    {
        shutdown();
    }

    bool D3D12TimestampSource::init(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t capacity)
    {
        shutdown();
        if (!device || !queue || capacity == 0)
        {
            return false;
        }

        const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            NAU_LOG_WARNING("GPU profiler: D3D12 timestamps are supported on direct and compute queues only");
            return false;
        }

        uint64_t frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
        {
            NAU_LOG_WARNING("GPU profiler: the queue does not provide timestamp frequency");
            return false;
        }

        D3D12_QUERY_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = capacity;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap))))
        {
            NAU_LOG_WARNING("GPU profiler: cannot create timestamp query heap");
            return false;
        }

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = uint64_t(capacity) * sizeof(uint64_t);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))))
        {
            NAU_LOG_WARNING("GPU profiler: cannot create timestamp readback buffer");
            return false;
        }

        // The readback buffer stays mapped: the CPU reads a slot only after the fence of its resolve has been passed.
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped)))
        {
            NAU_LOG_WARNING("GPU profiler: cannot map timestamp readback buffer");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            readback->Unmap(0, nullptr);
            NAU_LOG_WARNING("GPU profiler: cannot create timestamp fence");
            return false;
        }

        m_queue = queue;
        m_heap = heap;
        m_readback = readback;
        m_fence = fence;
        m_mapped = static_cast<const uint64_t*>(mapped);
        m_fenceValue = 0;
        m_frequency = frequency;

        m_slots.assign(capacity, Slot{});
        m_freeSlots.clear();
        m_freeSlots.reserve(capacity);
        for (uint32_t i = capacity; i > 0; --i)
        {
            m_freeSlots.push_back(i - 1);
        }
        return true;
    }

    void D3D12TimestampSource::shutdown()
    {
        if (m_readback && m_mapped)
        {
            const D3D12_RANGE noWrite = {0, 0};
            m_readback->Unmap(0, &noWrite);
        }
        m_mapped = nullptr;
        m_list = nullptr;
        m_queue.Reset();
        m_heap.Reset();
        m_readback.Reset();
        m_fence.Reset();
        m_fenceValue = 0;
        m_frequency = 0;
        m_slots.clear();
        m_freeSlots.clear();
        m_issued.clear();
        m_resolved.clear();
    }

    void D3D12TimestampSource::setCommandList(ID3D12GraphicsCommandList* list)
    {
        m_list = list;
    }

    void D3D12TimestampSource::resolve(ID3D12GraphicsCommandList* list)
    {
        if (!list || m_issued.empty())
        {
            return;
        }

        // One ResolveQueryData per run of consecutive slots; the value of slot i goes to offset i * 8.
        eastl::sort(m_issued.begin(), m_issued.end());
        size_t first = 0;
        while (first < m_issued.size())
        {
            size_t last = first;
            while (last + 1 < m_issued.size() && m_issued[last + 1] == m_issued[last] + 1)
            {
                ++last;
            }
            const uint32_t start = m_issued[first];
            const uint32_t count = m_issued[last] - start + 1;
            list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, start, count, m_readback.Get(),
                                   uint64_t(start) * sizeof(uint64_t));
            first = last + 1;
        }

        for (uint32_t slot : m_issued)
        {
            m_slots[slot].state = SlotState::Resolved;
            m_resolved.push_back(slot);
        }
        m_issued.clear();
    }

    void D3D12TimestampSource::submitted()
    {
        if (!m_queue || m_resolved.empty())
        {
            return;
        }

        ++m_fenceValue;
        if (FAILED(m_queue->Signal(m_fence.Get(), m_fenceValue)))
        {
            NAU_LOG_WARNING("GPU profiler: cannot signal timestamp fence");
            return;
        }
        for (uint32_t slot : m_resolved)
        {
            m_slots[slot].state = SlotState::Submitted;
            m_slots[slot].fenceValue = m_fenceValue;
        }
        m_resolved.clear();
    }

    uint64_t D3D12TimestampSource::getFrequency()
    {
        return m_frequency;
    }

    bool D3D12TimestampSource::canIssue()
    {
        return m_list != nullptr;
    }

    void D3D12TimestampSource::issue(void*& query)
    {
        if (!m_list)
        {
            return;
        }

        uint32_t slot = 0;
        if (query)
        {
            if (!toSlot(query, slot))
            {
                return;
            }
        }
        else
        {
            if (m_freeSlots.empty())
            {
                return;  // heap is full: the scope is not measured
            }
            slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            query = toQuery(slot);
        }

        m_list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
        m_slots[slot].state = SlotState::Issued;
        m_slots[slot].fenceValue = 0;
        m_issued.push_back(slot);
    }

    bool D3D12TimestampSource::tryGetValue(void* query, uint64_t& value)
    {
        uint32_t slot = 0;
        if (!toSlot(query, slot) || m_slots[slot].state != SlotState::Submitted)
        {
            return false;
        }
        if (m_fence->GetCompletedValue() < m_slots[slot].fenceValue)
        {
            return false;
        }
        value = m_mapped[slot];
        return true;
    }

    void D3D12TimestampSource::release(void* query)
    {
        uint32_t slot = 0;
        if (!toSlot(query, slot) || m_slots[slot].state == SlotState::Free)
        {
            return;
        }
        m_slots[slot] = {};
        m_issued.erase(eastl::remove(m_issued.begin(), m_issued.end(), slot), m_issued.end());
        m_resolved.erase(eastl::remove(m_resolved.begin(), m_resolved.end(), slot), m_resolved.end());
        m_freeSlots.push_back(slot);
    }

    bool D3D12TimestampSource::toSlot(void* query, uint32_t& slot) const
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(query);
        if (v == 0 || v > m_slots.size())
        {
            return false;
        }
        slot = static_cast<uint32_t>(v - 1);
        return true;
    }
}  // namespace nau::gpu_profiler
