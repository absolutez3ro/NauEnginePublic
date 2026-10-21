// Timestamp source for a GPU that is driven by raw D3D12 (not by the engine driver), e.g. a secondary GPU.
//
// Timestamps are written with EndQuery into a D3D12_QUERY_HEAP_TYPE_TIMESTAMP heap, resolved with ResolveQueryData into
// a readback buffer and become readable once a fence signaled after the command list has been passed by the queue.
// All timestamps are taken on one queue (direct or compute); the frequency is GetTimestampFrequency of that queue.
//
// ITimestampSource does not know about command lists, so the owner of the command list drives the source:
//
//     source.setCommandList(list);              // timers on this GPU write into `list` from now on
//     {
//         NAU_GPU_TIMER_ON(1, "copy");
//         list->CopyBufferRegion(...);
//     }
//     source.resolve(list);                     // before list->Close(): resolves every timestamp written into `list`
//     source.setCommandList(nullptr);           // timers on this GPU are skipped outside of recording
//     list->Close();
//     queue->ExecuteCommandLists(1, &list);
//     source.submitted();                       // after ExecuteCommandLists: signals the fence the values wait for
//
// The source must be used from one thread (the render thread, where the profiler runs).
#pragma once

#include <EASTL/vector.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "gpu_profiler.h"

namespace nau::gpu_profiler
{
    class D3D12TimestampSource final : public ITimestampSource
    {
    public:
        static constexpr uint32_t DefaultCapacity = 1024;

        D3D12TimestampSource() = default;
        ~D3D12TimestampSource() override;
        D3D12TimestampSource(const D3D12TimestampSource&) = delete;
        D3D12TimestampSource& operator=(const D3D12TimestampSource&) = delete;

        // `queue` must be a direct or compute queue of `device`; copy queues need a different query heap type.
        bool init(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t capacity = DefaultCapacity);
        void shutdown();
        bool isValid() const
        {
            return m_heap != nullptr;
        }

        // Command list that receives the timestamps of the following timers; nullptr stops measuring.
        void setCommandList(ID3D12GraphicsCommandList* list);
        // Records ResolveQueryData for every timestamp written since the previous resolve. Call before list->Close().
        void resolve(ID3D12GraphicsCommandList* list);
        // Signals the fence on the queue. Call after ExecuteCommandLists of the list passed to resolve().
        void submitted();

        // ITimestampSource
        uint64_t getFrequency() override;
        bool canIssue() override;
        void issue(void*& query) override;
        bool tryGetValue(void* query, uint64_t& value) override;
        void release(void* query) override;

    private:
        enum class SlotState : uint8_t
        {
            Free,
            Issued,     // EndQuery recorded, not resolved yet
            Resolved,   // ResolveQueryData recorded, list not submitted yet
            Submitted,  // waits for fenceValue
        };

        struct Slot
        {
            SlotState state = SlotState::Free;
            uint64_t fenceValue = 0;
        };

        static void* toQuery(uint32_t slot)
        {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(slot) + 1);
        }
        bool toSlot(void* query, uint32_t& slot) const;

        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        const uint64_t* m_mapped = nullptr;
        uint64_t m_fenceValue = 0;
        uint64_t m_frequency = 0;

        ID3D12GraphicsCommandList* m_list = nullptr;
        eastl::vector<Slot> m_slots;
        eastl::vector<uint32_t> m_freeSlots;
        eastl::vector<uint32_t> m_issued;    // written into the current command list
        eastl::vector<uint32_t> m_resolved;  // resolved, waiting for submitted()
    };
}  // namespace nau::gpu_profiler
