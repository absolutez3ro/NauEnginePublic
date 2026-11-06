// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "nau/gpu_profiler/d3d12_timestamp_source.h"

#include <dxgi1_4.h>

#include "profiler_log.h"

namespace nau::gpu_profiler
{
    namespace
    {
        void* toQuery(const uint32_t slot)
        {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(slot) + 1);
        }
    }  // namespace

    D3D12TimestampSource::D3D12TimestampSource() = default;

    D3D12TimestampSource::~D3D12TimestampSource()
    {
        shutdown();
    }

    bool D3D12TimestampSource::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* completionFence,
                                    const GpuIndex gpu, const char* queueName, const uint32_t capacity)
    {
        GpuProfiler& profiler = GpuProfiler::instance();
        if (!profiler.isEnabled() || isValid() || device == nullptr || queue == nullptr || completionFence == nullptr || capacity == 0)
        {
            return false;
        }

        const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        D3D12_QUERY_HEAP_TYPE heapType = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        if (queueDesc.Type == D3D12_COMMAND_LIST_TYPE_COPY)
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))) ||
                !options3.CopyQueueTimestampQueriesSupported)
            {
                logMessage(LogLevel::Warning, "copy queue '%s' on GPU %u: the adapter has no copy queue timestamps "
                           "(D3D12_OPTIONS3::CopyQueueTimestampQueriesSupported), it is not measured", queueName, gpu);
                return false;
            }
            heapType = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
        }
        else if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && queueDesc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            logMessage(LogLevel::Warning, "queue '%s' on GPU %u: unsupported queue type %d, it is not measured", queueName, gpu,
                       static_cast<int>(queueDesc.Type));
            return false;
        }

        UINT64 frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
        {
            logMessage(LogLevel::Warning, "queue '%s' on GPU %u: GetTimestampFrequency failed, it is not measured", queueName, gpu);
            return false;
        }

        D3D12_QUERY_HEAP_DESC heapDesc = {};
        heapDesc.Type = heapType;
        heapDesc.Count = capacity;

        D3D12_HEAP_PROPERTIES readbackHeap = {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = static_cast<UINT64>(capacity) * sizeof(uint64_t);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        void* mapped = nullptr;
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_heap))) ||
            FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&m_readback))) ||
            FAILED(m_readback->Map(0, nullptr, &mapped)))
        {
            logMessage(LogLevel::Error, "queue '%s' on GPU %u: could not create the timestamp objects, it is not measured", queueName, gpu);
            m_heap.Reset();
            m_readback.Reset();
            return false;
        }
        m_heap->SetName(L"GpuProfiler Timestamps");
        m_readback->SetName(L"GpuProfiler Timestamps Readback");

        m_queue = queue;
        m_fence = completionFence;
        m_mapped = static_cast<const uint64_t*>(mapped);
        m_frequency = frequency;
        m_slots.assign(capacity, Slot{});
        m_freeSlots.clear();
        for (uint32_t i = capacity; i > 0; --i)
        {
            m_freeSlots.push_back(i - 1);
        }

        m_id = profiler.registerSource(gpu, queueName, this);
        if (m_id == InvalidSource)
        {
            shutdown();
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc = {};
        char name[sizeof(desc.Description) * 2] = {};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) &&
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr) > 0)
        {
            profiler.setGpuName(gpu, name);
        }
        return true;
    }

    void D3D12TimestampSource::shutdown()
    {
        if (m_id != InvalidSource)
        {
            const SourceId id = m_id;
            m_id = InvalidSource;
            GpuProfiler::instance().unregisterSource(id);
        }
        if (m_readback && m_mapped != nullptr)
        {
            const D3D12_RANGE noWrite = {0, 0};
            m_readback->Unmap(0, &noWrite);
        }
        m_mapped = nullptr;
        m_list = nullptr;
        m_heap.Reset();
        m_readback.Reset();
        m_fence.Reset();
        m_queue.Reset();
        m_slots.clear();
        m_freeSlots.clear();
        m_issued.clear();
        m_resolved.clear();
        m_released.clear();
    }

    void D3D12TimestampSource::setCommandList(ID3D12GraphicsCommandList* list)
    {
        m_list = list;
    }

    void D3D12TimestampSource::resolve()
    {
        if (m_list == nullptr || m_issued.empty())
        {
            return;
        }
        for (const uint32_t slot : m_issued)
        {
            m_list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot, 1, m_readback.Get(),
                                     static_cast<UINT64>(slot) * sizeof(uint64_t));
            if (m_slots[slot].state == SlotState::Issued)
            {
                m_slots[slot].state = SlotState::Resolved;
            }
            m_resolved.push_back(slot);
        }
        m_issued.clear();
    }

    void D3D12TimestampSource::submitted(const uint64_t fenceValue)
    {
        for (const uint32_t slot : m_resolved)
        {
            // A released slot stays released; it becomes free once this submission has passed.
            if (m_slots[slot].state != SlotState::Released)
            {
                m_slots[slot].state = SlotState::Submitted;
            }
            m_slots[slot].fenceValue = fenceValue;
        }
        m_resolved.clear();
    }

    void D3D12TimestampSource::cancelRecording()
    {
        m_list = nullptr;
        for (auto* queries : {&m_issued, &m_resolved})
        {
            for (const uint32_t slot : *queries)
            {
                auto& entry = m_slots[slot];
                if (entry.state == SlotState::Released)
                {
                    entry.fenceValue = 0;
                }
                else
                {
                    entry.state = SlotState::Cancelled;
                }
            }
            queries->clear();
        }
    }

    uint64_t D3D12TimestampSource::getFrequency()
    {
        return m_frequency;
    }

    bool D3D12TimestampSource::canIssue()
    {
        return m_list != nullptr && m_heap;
    }

    void D3D12TimestampSource::issue(void*& query)
    {
        uint32_t slot = 0;
        if (!canIssue())
        {
            return;
        }
        // The profiler hands back queries whose value it has read: such a slot is reused as it is.
        const bool reusable = toSlot(query, slot) &&
                              (m_slots[slot].state == SlotState::Free ||
                               (m_slots[slot].state == SlotState::Submitted && m_fence->GetCompletedValue() >= m_slots[slot].fenceValue));
        if (!reusable)
        {
            reclaimReleased();
            if (m_freeSlots.empty())
            {
                query = nullptr; // never resolved: the scope times out in the profiler
                return;
            }
            slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            query = toQuery(slot);
        }
        m_list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
        m_slots[slot].state = SlotState::Issued;
        m_issued.push_back(slot);
    }

    bool D3D12TimestampSource::tryGetValue(void* query, uint64_t& value)
    {
        uint32_t slot = 0;
        if (!toSlot(query, slot) || m_slots[slot].state != SlotState::Submitted)
        {
            return false;
        }
        const uint64_t completed = m_fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < m_slots[slot].fenceValue)
        {
            return false;
        }
        value = m_mapped[slot];
        return true;
    }

    void D3D12TimestampSource::release(void* query)
    {
        uint32_t slot = 0;
        if (!toSlot(query, slot))
        {
            return;
        }
        Slot& entry = m_slots[slot];
        switch (entry.state)
        {
            case SlotState::Submitted:
                // ResolveQueryData may still write it.
                entry.state = SlotState::Released;
                m_released.push_back(slot);
                break;
            case SlotState::Cancelled:
                entry = Slot{};
                m_freeSlots.push_back(slot);
                break;
            case SlotState::Issued:
            case SlotState::Resolved:
                // Issued or resolved into a list that will still be submitted: wait for that submission.
                entry.state = SlotState::Released;
                entry.fenceValue = ~0ull;
                m_released.push_back(slot);
                break;
            default:
                break;
        }
    }

    bool D3D12TimestampSource::getClockCalibration(uint64_t& gpuTicks, uint64_t& cpuTicks, uint64_t& cpuFrequency)
    {
        UINT64 gpu = 0;
        UINT64 cpu = 0;
        if (!m_queue || FAILED(m_queue->GetClockCalibration(&gpu, &cpu)))
        {
            return false;
        }
        gpuTicks = gpu;
        cpuTicks = cpu;
        cpuFrequency = 0; // QueryPerformanceCounter
        return true;
    }

    bool D3D12TimestampSource::toSlot(void* query, uint32_t& slot) const
    {
        const auto value = reinterpret_cast<uintptr_t>(query);
        if (value == 0 || value > m_slots.size())
        {
            return false;
        }
        slot = static_cast<uint32_t>(value - 1);
        return true;
    }

    void D3D12TimestampSource::reclaimReleased()
    {
        if (m_released.empty())
        {
            return;
        }
        const uint64_t completed = m_fence->GetCompletedValue();
        for (size_t i = 0; i < m_released.size();)
        {
            const uint32_t slot = m_released[i];
            if (m_slots[slot].fenceValue <= completed)
            {
                m_slots[slot] = Slot{};
                m_freeSlots.push_back(slot);
                m_released[i] = m_released.back();
                m_released.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }
}  // namespace nau::gpu_profiler
