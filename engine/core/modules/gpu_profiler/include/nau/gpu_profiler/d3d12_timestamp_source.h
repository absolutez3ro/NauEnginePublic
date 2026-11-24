// d3d12_timestamp_source.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

// Timestamp source for a native D3D12 queue outside Nau's driver command stream.
//
// The source owns a query heap and a readback buffer and observes the queue owner's completion fence.
// It is bound to one queue:
// TIMESTAMP heap for direct and compute queues, COPY_QUEUE_TIMESTAMP for copy queues (when
// D3D12_FEATURE_D3D12_OPTIONS3::CopyQueueTimestampQueriesSupported; otherwise the queue is not measured and the reason
// is logged). The frequency is GetTimestampFrequency of that queue.
//
// The owner of the command list drives the source:
//
//     source.setCommandList(list);         // timers of this source write into `list` from now on
//     { NAU_GPU_TIMER_SRC(source.id(), "pass"); ... }
//     source.resolve();                    // before list->Close(): ResolveQueryData of what was written into `list`
//     source.setCommandList(nullptr);      // timers of this source are skipped outside of recording
//     ... ExecuteCommandLists ...
//     source.submitted(fenceValue);         // associate queries with the owner's completion signal for this list
//
// The results become readable when the fence has been passed; nothing waits for it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "nau/gpu_profiler/gpu_profiler.h"

namespace nau::gpu_profiler
{
    class D3D12TimestampSource final : public ITimestampSource
    {
    public:
        static constexpr uint32_t DefaultCapacity = 512;

        NAU_GPU_PROFILER_API D3D12TimestampSource();
        NAU_GPU_PROFILER_API ~D3D12TimestampSource() override;
        /**
         * @brief Records adapter metadata for the profiler, including for Nau's primary device.
         * @param device Native DX12 device to identify.
         * @param gpu Profiler GPU index for the device.
         */
        NAU_GPU_PROFILER_API static void setDeviceInfo(ID3D12Device* device, GpuIndex gpu);

        D3D12TimestampSource(const D3D12TimestampSource&) = delete;
        D3D12TimestampSource& operator=(const D3D12TimestampSource&) = delete;

        /**
         * @brief Creates a timestamp source for one D3D12 queue and registers it with the profiler.
         * @param device Device that owns the queue.
         * @param queue Queue whose timestamps are measured.
         * @param completionFence Queue owner's fence used to check whether submitted queries can be read.
         * @param gpu Profiler GPU index for the queue.
         * @param queueName Queue label recorded in the profiler output.
         * @param capacity Maximum number of timestamp slots.
         * @return True if registered. False if profiling is disabled, unsupported or initialization fails.
         */
        NAU_GPU_PROFILER_API bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* completionFence, GpuIndex gpu, const char* queueName, uint32_t capacity = DefaultCapacity);
        /** @brief Unregisters the source before the device or queue is destroyed. */
        void shutdown();

        bool isValid() const
        {
            return m_id != InvalidSource;
        }
        /** @brief Returns the source ID for NAU_GPU_TIMER_SRC, or InvalidSource when inactive. */
        SourceId id() const
        {
            return m_id;
        }

        /**
         * @brief Sets the command list that receives timestamp writes.
         * @param list Borrowed recording command list, or nullptr to stop recording.
         */
        NAU_GPU_PROFILER_API void setCommandList(ID3D12GraphicsCommandList* list);
        /** @brief Records timestamp readback for the current command list before it is closed. */
        NAU_GPU_PROFILER_API void resolve();
        /**
         * @brief Associates resolved timestamps with a submitted command list.
         * @param fenceValue Completion fence value signaled after execution of the list.
         */
        NAU_GPU_PROFILER_API void submitted(uint64_t fenceValue);
        /** @brief Discards timestamp writes from an abandoned command list. */
        NAU_GPU_PROFILER_API void cancelRecording();

        // ITimestampSource
        uint64_t getFrequency() override;
        bool canIssue() override;
        void issue(void*& query) override;
        bool tryGetValue(void* query, uint64_t& value) override;
        void release(void* query) override;
        bool getClockCalibration(uint64_t& gpuTicks, uint64_t& cpuTicks, uint64_t& cpuFrequency) override;

    private:
        enum class SlotState : uint8_t
        {
            Free,
            Issued,     // EndQuery recorded, not resolved yet
            Resolved,   // ResolveQueryData recorded, the list is not submitted yet
            Submitted,  // waits for fenceValue
            Cancelled,  // the profiler still holds a query from a list that was abandoned
            Released    // given back while the GPU may still write it: free once fenceValue has passed
        };

        struct Slot
        {
            SlotState state = SlotState::Free;
            uint64_t fenceValue = 0;
        };

        bool toSlot(void* query, uint32_t& slot) const;
        void reclaimReleased();

        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        const uint64_t* m_mapped = nullptr;
        uint64_t m_frequency = 0;
        SourceId m_id = InvalidSource;

        ID3D12GraphicsCommandList* m_list = nullptr;
        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_freeSlots;
        std::vector<uint32_t> m_issued;
        std::vector<uint32_t> m_resolved;
        std::vector<uint32_t> m_released;
    };
}  // namespace nau::gpu_profiler
