// multi_gpu_transfer.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
//
#pragma once

#include <EASTL/shared_ptr.h>
#include <wrl/client.h>

#include <cstdint>
#include <cwchar>

#include "multi_gpu_fence.h"
#include "nau/directx/d3d12.h"

namespace d3d::mgpu
{
    struct TransferChannel;
}

namespace drv3d_dx12
{
    struct SharedTextureRead
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    };

    SharedTextureRead getSecondarySharedInput(d3d::mgpu::TransferChannel* channel, uint64_t ticket, ID3D12Resource* destination);

    /**
     * @brief A fence and a value on one device. Queues of that device may wait for it on the GPU.
     */
    struct CrossAdapterSyncPoint
    {
        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
    };

    /**
     * @brief Repeatedly moves one 2D image from a producer device to a consumer device.
     *
     * Data goes through slots in a heap shared across the adapters: the producer copies its resource into a slot on its own
     * copy queue, the consumer copies the slot into its resource on its own copy queue. Slots are buffers with a placed
     * footprint, because cross-adapter row-major textures are often unsupported or slower. Each side is ordered with a
     * CrossAdapterFence, so neither call blocks the CPU.
     *
     * Endpoint resources must be in D3D12_RESOURCE_STATE_COMMON when a copy starts and decay back to it afterwards, as
     * copy queues require. Only subresource 0 of a texture is moved. The channel does not own endpoints: keep a source
     * alive until its sourceReleased point and a destination until its receive point is reached.
     *
     * Slot reuse: ticket t occupies slot t % slot_count. Before reusing a slot, the producer waits on the GPU for the last
     * receive of its frame. A frame that was never received but is older than a received one is skipped without a wait.
     * If neither holds, the consumer is late and the new frame is dropped (send returns ticket 0) rather than stalling the
     * producer for a receive that may never come.
     *
     * Experimental synchronization=false uses one shared slot and omits cross-adapter read/write ordering.
     * Endpoint dependencies, command allocator fences and destruction drains still apply.
     *
     * Nothing is allocated per call: slots and command lists come from fixed pools. If the CPU runs more than
     * MAX_RECORDINGS submissions ahead of a GPU, further calls fail instead of blocking.
     *
     * Not thread safe: use from one thread.
     */
    class CrossAdapterChannel
    {
    public:
        static constexpr uint32_t MAX_SLOTS = 4;
        static constexpr uint32_t MAX_RECORDINGS = 8;

        struct Send
        {
            uint64_t ticket = 0;                   ///< 0 if nothing was sent.
            CrossAdapterSyncPoint sourceReleased;  ///< On the producer: the copy has finished reading the source.
        };

    private:
        enum End : uint32_t
        {
            PRODUCER,
            CONSUMER,
            END_COUNT
        };

        struct Recording
        {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            uint64_t fenceValue = 0;  ///< Value of this end's signaling fence after which the allocator may be reset.
        };

        struct Side
        {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            Microsoft::WRL::ComPtr<ID3D12Heap> heap;
            Microsoft::WRL::ComPtr<ID3D12Resource> slots[MAX_SLOTS];
            Recording recordings[MAX_RECORDINGS];
        };

        struct Slot
        {
            uint64_t ticket = 0;
            uint64_t writtenValue = 0;
            uint64_t consumedValue = 0;  ///< 0 while no receive of this ticket was issued.
        };

        Side m_sides[END_COUNT];
        CrossAdapterFence m_written;   // producer signals after a slot is filled
        CrossAdapterFence m_consumed;  // consumer signals after a slot is read
        Slot m_slotStates[MAX_SLOTS];
        uint32_t m_slotCount = 0;
        bool m_synchronization = true;
        D3D12_RESOURCE_DESC m_payload = {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint = {};
        uint64_t m_slotSize = 0;
        uint64_t m_lastSent = 0;
        uint64_t m_lastReceived = 0;

        static bool sameDevice(ID3D12DeviceChild* child, ID3D12Device* device)
        {
            Microsoft::WRL::ComPtr<IUnknown> owner;
            Microsoft::WRL::ComPtr<IUnknown> expected;
            return child && device && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) &&
                   SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) && owner.Get() == expected.Get();
        }

        static bool isTexture(const D3D12_RESOURCE_DESC& desc)
        {
            return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }

        static bool isSupportedPayload(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
        {
            if (!isTexture(desc) || desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1)
            {
                return false;
            }
            D3D12_FEATURE_DATA_FORMAT_INFO info = {desc.Format, 0};
            return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) && info.PlaneCount == 1;
        }

        // Texture endpoints may have more mips or slices than the m_payload; subresource 0 has to have its size and byte layout.
        // The format itself may differ, e.g. a typeless engine texture on one side and a typed one on the other: the slot
        // holds raw bytes, and each side copies with a m_footprint in its own format, which is always copy-compatible.
        bool matchesPayload(ID3D12Resource* resource, End end, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout) const
        {
            if (!resource || !sameDevice(resource, m_sides[end].device.Get()))
            {
                return false;
            }
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            if (desc.Dimension != m_payload.Dimension)
            {
                return false;
            }

            if (desc.Width != m_payload.Width || desc.Height != m_payload.Height || desc.SampleDesc.Count != 1)
            {
                return false;
            }
            uint64_t total = 0;
            m_sides[end].device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
            return total == m_slotSize && layout.Footprint.RowPitch == m_footprint.Footprint.RowPitch;
        }

        HRESULT initSide(End end, const D3D12_RESOURCE_DESC& slot_desc, uint64_t slot_stride, const wchar_t* name)
        {
            Side& side = m_sides[end];
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            HRESULT hr = side.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&side.queue));
            if (FAILED(hr))
            {
                return hr;
            }
            for (uint32_t i = 0; i < m_slotCount; ++i)
            {
                hr = side.device->CreatePlacedResource(side.heap.Get(), i * slot_stride, &slot_desc, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&side.slots[i]));
                if (FAILED(hr))
                {
                    return hr;
                }
            }
            if (name)
            {
                wchar_t label[128];
                swprintf(label, 128, L"%ls %ls copy", name, end == PRODUCER ? L"send" : L"receive");
                side.queue->SetName(label);
                for (uint32_t i = 0; i < m_slotCount; ++i)
                {
                    swprintf(label, 128, L"%ls slot %u", name, i);
                    side.slots[i]->SetName(label);
                }
            }
            return S_OK;
        }

        // Returns an open command list whose previous submission has finished on the GPU, or nullptr if all are in flight.
        ID3D12GraphicsCommandList* beginRecording(End end, Recording*& recording)
        {
            Side& side = m_sides[end];
            const uint64_t completed = (end == PRODUCER ? m_written : m_consumed).completedValue();
            recording = nullptr;
            for (Recording& candidate : side.recordings)
            {
                if (candidate.fenceValue <= completed)
                {
                    recording = &candidate;
                    break;
                }
            }
            if (!recording)
            {
                return nullptr;
            }
            if (!recording->allocator)
            {
                if (FAILED(side.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&recording->allocator))) ||
                    FAILED(side.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, recording->allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&recording->list))))
                {
                    *recording = {};
                    return nullptr;
                }
                return recording->list.Get();
            }
            if (FAILED(recording->allocator->Reset()) || FAILED(recording->list->Reset(recording->allocator.Get(), nullptr)))
            {
                return nullptr;
            }
            return recording->list.Get();
        }

        // Closes and executes the list, then signals fence on this end's queue. Returns the signaled value or 0.
        uint64_t submit(End end, Recording& recording, CrossAdapterFence& fence)
        {
            if (FAILED(recording.list->Close()))
            {
                return 0;
            }
            ID3D12CommandList* lists[] = {recording.list.Get()};
            m_sides[end].queue->ExecuteCommandLists(1, lists);

            const uint64_t value = fence.enqueueSignal(m_sides[end].queue.Get());
            // A failed signal leaves no value to wait for; keep the allocator out of rotation for good.
            recording.fenceValue = value ? value : UINT64_MAX;
            return value;
        }

        void recordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout, ID3D12Resource* slot, bool into_slot) const
        {
            D3D12_TEXTURE_COPY_LOCATION texture = {};
            texture.pResource = resource;
            texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            texture.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION placed = {};
            placed.pResource = slot;
            placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            placed.PlacedFootprint = layout;
            if (into_slot)
            {
                list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
            }
            else
            {
                list->CopyTextureRegion(&texture, 0, 0, 0, &placed, nullptr);
            }
        }

        bool enqueueExternalWait(End end, const CrossAdapterSyncPoint& point)
        {
            if (!point.fence)
            {
                return true;
            }
            return sameDevice(point.fence, m_sides[end].device.Get()) && SUCCEEDED(m_sides[end].queue->Wait(point.fence, point.value));
        }

    public:
        CrossAdapterChannel() = default;
        CrossAdapterChannel(const CrossAdapterChannel&) = delete;
        CrossAdapterChannel(CrossAdapterChannel&&) = delete;
        CrossAdapterChannel& operator=(const CrossAdapterChannel&) = delete;
        CrossAdapterChannel& operator=(CrossAdapterChannel&&) = delete;
        ~CrossAdapterChannel()
        {
            reset();
        }

        /**
         * @brief Creates the shared slots, the copy queues and the fences.
         * @param producer Device that sends.
         * @param consumer Device that receives, on another adapter.
         * @param heap_owner producer or consumer: the device that creates the shared heap.
         * @param payload_desc Description of a 2D texture: single plane, no MSAA; subresource 0 is moved.
         * @param slot_count From 2 to MAX_SLOTS with synchronization, exactly 1 without it.
         * @param name Debug name prefix for the queues and slots, may be nullptr.
         * @return S_OK, E_INVALIDARG for unsupported input, DXGI_ERROR_UNSUPPORTED if the adapters disagree on the copy
         *         layout, or the failing HRESULT. On failure the object stays empty.
         */
        HRESULT init(ID3D12Device* producer, ID3D12Device* consumer, ID3D12Device* heap_owner, const D3D12_RESOURCE_DESC& payload_desc, uint32_t slot_count, const wchar_t* name, bool synchronization = true)
        {
            reset();
            if (!producer || !consumer || producer == consumer || (heap_owner != producer && heap_owner != consumer) ||
                slot_count < (synchronization ? 2u : 1u) || slot_count > MAX_SLOTS || (!synchronization && slot_count != 1) || !isSupportedPayload(producer, payload_desc) || !isSupportedPayload(consumer, payload_desc))
            {
                return E_INVALIDARG;
            }

            uint64_t payloadSize = 0;
            {
                // Both adapters must read the slot with the same layout the other one wrote.
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[END_COUNT] = {};
                uint64_t totals[END_COUNT] = {};
                producer->GetCopyableFootprints(&payload_desc, 0, 1, 0, &layouts[PRODUCER], nullptr, nullptr, &totals[PRODUCER]);
                consumer->GetCopyableFootprints(&payload_desc, 0, 1, 0, &layouts[CONSUMER], nullptr, nullptr, &totals[CONSUMER]);
                if (totals[PRODUCER] == UINT64_MAX || totals[PRODUCER] != totals[CONSUMER] ||
                    layouts[PRODUCER].Footprint.RowPitch != layouts[CONSUMER].Footprint.RowPitch)
                {
                    return DXGI_ERROR_UNSUPPORTED;
                }
                m_footprint = layouts[PRODUCER];
                payloadSize = totals[PRODUCER];
            }

            D3D12_RESOURCE_DESC slotDesc = {};
            slotDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            slotDesc.Width = payloadSize;
            slotDesc.Height = 1;
            slotDesc.DepthOrArraySize = 1;
            slotDesc.MipLevels = 1;
            slotDesc.SampleDesc.Count = 1;
            slotDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            slotDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            const D3D12_RESOURCE_ALLOCATION_INFO allocation = heap_owner->GetResourceAllocationInfo(0, 1, &slotDesc);
            if (allocation.SizeInBytes == UINT64_MAX)
            {
                return E_INVALIDARG;
            }
            const uint64_t alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            const uint64_t slotStride = (allocation.SizeInBytes + alignment - 1) / alignment * alignment;

            m_sides[PRODUCER].device = producer;
            m_sides[CONSUMER].device = consumer;
            m_slotCount = slot_count;
            m_synchronization = synchronization;
            m_slotSize = payloadSize;
            m_payload = payload_desc;

            const End owner = heap_owner == producer ? PRODUCER : CONSUMER;
            const End other = owner == PRODUCER ? CONSUMER : PRODUCER;
            D3D12_HEAP_DESC heapDesc = {};
            heapDesc.SizeInBytes = slotStride * slot_count;
            heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
            HRESULT hr = heap_owner->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_sides[owner].heap));
            HANDLE handle = nullptr;
            if (SUCCEEDED(hr))
            {
                hr = heap_owner->CreateSharedHandle(m_sides[owner].heap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_sides[other].device->OpenSharedHandle(handle, IID_PPV_ARGS(&m_sides[other].heap));
                CloseHandle(handle);
            }
            if (SUCCEEDED(hr))
            {
                hr = initSide(PRODUCER, slotDesc, slotStride, name);
            }
            if (SUCCEEDED(hr))
            {
                hr = initSide(CONSUMER, slotDesc, slotStride, name);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_written.init(producer, consumer);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_consumed.init(consumer, producer);
            }
            if (SUCCEEDED(hr) && name)
            {
                m_sides[owner].heap->SetName(name);
            }
            if (FAILED(hr))
            {
                reset();
            }
            return hr;
        }

        /**
         * @brief Waits on the CPU until both GPUs are done with the channel, then releases everything.
         */
        void reset()
        {
            if (m_written)
            {
                m_written.waitOnCpu(m_written.issuedValue(), INFINITE);
            }
            if (m_consumed)
            {
                m_consumed.waitOnCpu(m_consumed.issuedValue(), INFINITE);
            }

            for (Side& side : m_sides)
            {
                side = {};
            }
            m_written.reset();
            m_consumed.reset();
            for (Slot& slot : m_slotStates)
            {
                slot = {};
            }
            m_slotCount = 0;
            m_synchronization = true;
            m_payload = {};
            m_footprint = {};
            m_slotSize = 0;
            m_lastSent = 0;
            m_lastReceived = 0;
        }

        explicit operator bool() const
        {
            return static_cast<bool>(m_written);
        }

        /**
         * @brief False if send() would drop the frame because the consumer is late. Has no side effects, so a caller can
         *        skip preparing a source that would not be sent.
         */
        bool isNextSlotFree() const
        {
            if (!*this)
            {
                return false;
            }
            const Slot& slot = m_slotStates[(m_lastSent + 1) % m_slotCount];
            return !m_synchronization || slot.ticket == 0 || slot.consumedValue != 0 || m_lastReceived >= slot.ticket;
        }

        /**
         * @brief True if receive() can still copy ticket, i.e. it was sent and its slot was not reused.
         */
        bool holds(uint64_t ticket) const
        {
            return *this && ticket != 0 && m_slotStates[ticket % m_slotCount].ticket == ticket;
        }

        /**
         * @brief Polls the source copy without inserting a queue wait. Removed devices never publish valid data.
         */
        bool isReady(uint64_t ticket) const
        {
            return holds(ticket) && !isLost() && m_written.isCompleted(m_slotStates[ticket % m_slotCount].writtenValue);
        }

        // The technique records this read on its own queue. Keep both resource and heap alive until that queue finishes.
        SharedTextureRead getSharedRead(uint64_t ticket, ID3D12Resource* destination) const
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
            if (m_synchronization || !holds(ticket) || !matchesPayload(destination, CONSUMER, layout))
            {
                return {};
            }
            return {m_sides[CONSUMER].slots[ticket % m_slotCount], m_sides[CONSUMER].heap, layout};
        }

        /**
         * @brief Copies source on the producer into the next slot.
         * @param source Producer resource matching the m_payload, in COMMON when the copy starts.
         * @param source_ready Producer fence the copy waits for, e.g. the end of the pass that writes source; may be empty.
         * @return Ticket for receive() and the point after which source may be m_written again; ticket 0 if the input is
         *         invalid, the consumer is late (the frame is dropped) or submission failed.
         */
        Send send(ID3D12Resource* source, CrossAdapterSyncPoint source_ready)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
            if (!*this || !matchesPayload(source, PRODUCER, layout) ||
                (source_ready.fence && !sameDevice(source_ready.fence, m_sides[PRODUCER].device.Get())))
            {
                return {};
            }
            if (!isNextSlotFree())
            {
                return {};
            }
            const uint64_t ticket = m_lastSent + 1;
            Slot& slot = m_slotStates[ticket % m_slotCount];

            Recording* recording = nullptr;
            ID3D12GraphicsCommandList* list = beginRecording(PRODUCER, recording);
            if (!list)
            {
                return {};
            }
            if (!enqueueExternalWait(PRODUCER, source_ready) ||
                (m_synchronization && slot.consumedValue != 0 && !m_consumed.enqueueWait(m_sides[PRODUCER].queue.Get(), slot.consumedValue)))
            {
                list->Close();
                return {};
            }

            recordCopy(list, source, layout, m_sides[PRODUCER].slots[ticket % m_slotCount].Get(), true);

            const uint64_t value = submit(PRODUCER, *recording, m_written);
            if (!value)
            {
                slot = {};  // the copy may have overwritten the frame that was there
                return {};
            }

            slot = {ticket, value, 0};
            m_lastSent = ticket;
            return {
                ticket,
                {m_written.getFence(CrossAdapterFence::Side::SIGNALER), value}
            };
        }

        /**
         * @brief Copies the slot holding ticket into destination on the consumer.
         * @param ticket Value returned by send(); it can be received more than once until its slot is reused.
         * @param destination Consumer resource matching the m_payload, in COMMON when the copy starts.
         * @param destination_free Consumer fence the copy waits for, e.g. the last pass reading destination; may be empty.
         * @return Point on the consumer after which destination holds the data; empty if the ticket is unknown or overwritten,
         *         the input is invalid or submission failed.
         */
        CrossAdapterSyncPoint receive(uint64_t ticket, ID3D12Resource* destination, CrossAdapterSyncPoint destination_free)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
            if (!*this || ticket == 0 || !matchesPayload(destination, CONSUMER, layout) ||
                (destination_free.fence && !sameDevice(destination_free.fence, m_sides[CONSUMER].device.Get())))
            {
                return {};
            }
            if (!holds(ticket))
            {
                return {};
            }
            Slot& slot = m_slotStates[ticket % m_slotCount];

            Recording* recording = nullptr;
            ID3D12GraphicsCommandList* list = beginRecording(CONSUMER, recording);
            if (!list)
            {
                return {};
            }
            if (!enqueueExternalWait(CONSUMER, destination_free) ||
                (m_synchronization && !m_written.enqueueWait(m_sides[CONSUMER].queue.Get(), slot.writtenValue)))
            {
                list->Close();
                return {};
            }

            recordCopy(list, destination, layout, m_sides[CONSUMER].slots[ticket % m_slotCount].Get(), false);

            const uint64_t value = submit(CONSUMER, *recording, m_consumed);
            if (!value)
            {
                return {};
            }

            slot.consumedValue = value;
            if (ticket > m_lastReceived)
            {
                m_lastReceived = ticket;
            }
            return {m_consumed.getFence(CrossAdapterFence::Side::SIGNALER), value};
        }

        /**
         * @brief True once either device is removed. Waits stop holding the queues then, so the data is no longer valid.
         */
        bool isLost() const
        {
            return m_written.isSignalerLost() || m_consumed.isSignalerLost();
        }
    };
}  // namespace drv3d_dx12
