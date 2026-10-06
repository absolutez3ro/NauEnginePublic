// Copyright 2024 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include <cwchar>
#include "multi_gpu_fence.h"
#include "nau/directx/d3d12.h"
#include <wrl/client.h>

namespace drv3d_dx12
{
/**
 * @brief A fence and a value on one device. Queues of that device may wait for it on the GPU.
 */
struct CrossAdapterSyncPoint
{
  ID3D12Fence *fence = nullptr;
  uint64_t value = 0;
};

/**
 * @brief Repeatedly moves one 2D image or one buffer from a producer device to a consumer device.
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
    uint64_t ticket = 0; ///< 0 if nothing was sent.
    CrossAdapterSyncPoint sourceReleased; ///< On the producer: the copy has finished reading the source.
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
    uint64_t fenceValue = 0; ///< Value of this end's signaling fence after which the allocator may be reset.
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
    uint64_t consumedValue = 0; ///< 0 while no receive of this ticket was issued.
  };

  Side sides[END_COUNT];
  CrossAdapterFence written;  // producer signals after a slot is filled
  CrossAdapterFence consumed; // consumer signals after a slot is read
  Slot slotStates[MAX_SLOTS];
  uint32_t slotCount = 0;
  D3D12_RESOURCE_DESC payload = {};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  uint64_t slotSize = 0;
  uint64_t lastSent = 0;
  uint64_t lastReceived = 0;

  static bool sameDevice(ID3D12DeviceChild *child, ID3D12Device *device)
  {
    Microsoft::WRL::ComPtr<IUnknown> owner;
    Microsoft::WRL::ComPtr<IUnknown> expected;
    return child && device && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) &&
           SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) && owner.Get() == expected.Get();
  }

  static bool isTexture(const D3D12_RESOURCE_DESC &desc) { return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D; }

  static bool isSupportedPayload(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc)
  {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return desc.Width > 0;
    if (!isTexture(desc) || desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1)
      return false;
    D3D12_FEATURE_DATA_FORMAT_INFO info = {desc.Format, 0};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) && info.PlaneCount == 1;
  }

  // Texture endpoints may have more mips or slices than the payload; subresource 0 has to have its size and byte layout.
  // The format itself may differ, e.g. a typeless engine texture on one side and a typed one on the other: the slot
  // holds raw bytes, and each side copies with a footprint in its own format, which is always copy-compatible.
  // Buffer endpoints may be larger and hold the payload at an offset, as sub-allocated engine buffers do.
  bool matchesPayload(ID3D12Resource *resource, uint64_t offset, End end, D3D12_PLACED_SUBRESOURCE_FOOTPRINT &layout) const
  {
    if (!resource || !sameDevice(resource, sides[end].device.Get()))
      return false;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != payload.Dimension)
      return false;
    if (!isTexture(payload))
      return offset <= desc.Width && payload.Width <= desc.Width - offset;
    if (offset != 0 || desc.Width != payload.Width || desc.Height != payload.Height || desc.SampleDesc.Count != 1)
      return false;
    uint64_t total = 0;
    sides[end].device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
    return total == slotSize && layout.Footprint.RowPitch == footprint.Footprint.RowPitch;
  }

  HRESULT initSide(End end, const D3D12_RESOURCE_DESC &slot_desc, uint64_t slot_stride, const wchar_t *name)
  {
    Side &side = sides[end];
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    HRESULT hr = side.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&side.queue));
    if (FAILED(hr))
      return hr;
    for (uint32_t i = 0; i < slotCount; ++i)
    {
      hr = side.device->CreatePlacedResource(side.heap.Get(), i * slot_stride, &slot_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&side.slots[i]));
      if (FAILED(hr))
        return hr;
    }
    if (name)
    {
      wchar_t label[128];
      swprintf(label, 128, L"%ls %ls copy", name, end == PRODUCER ? L"send" : L"receive");
      side.queue->SetName(label);
      for (uint32_t i = 0; i < slotCount; ++i)
      {
        swprintf(label, 128, L"%ls slot %u", name, i);
        side.slots[i]->SetName(label);
      }
    }
    return S_OK;
  }

  // Returns an open command list whose previous submission has finished on the GPU, or nullptr if all are in flight.
  ID3D12GraphicsCommandList *beginRecording(End end, Recording *&recording)
  {
    Side &side = sides[end];
    const uint64_t completed = (end == PRODUCER ? written : consumed).completedValue();
    recording = nullptr;
    for (Recording &candidate : side.recordings)
      if (candidate.fenceValue <= completed)
      {
        recording = &candidate;
        break;
      }
    if (!recording)
      return nullptr;
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
      return nullptr;
    return recording->list.Get();
  }

  // Closes and executes the list, then signals fence on this end's queue. Returns the signaled value or 0.
  uint64_t submit(End end, Recording &recording, CrossAdapterFence &fence)
  {
    if (FAILED(recording.list->Close()))
      return 0;
    ID3D12CommandList *lists[] = {recording.list.Get()};
    sides[end].queue->ExecuteCommandLists(1, lists);
    const uint64_t value = fence.enqueueSignal(sides[end].queue.Get());
    // A failed signal leaves no value to wait for; keep the allocator out of rotation for good.
    recording.fenceValue = value ? value : UINT64_MAX;
    return value;
  }

  void recordCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, uint64_t offset,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &layout, ID3D12Resource *slot, bool into_slot) const
  {
    if (!isTexture(payload))
    {
      if (into_slot)
        list->CopyBufferRegion(slot, 0, resource, offset, payload.Width);
      else
        list->CopyBufferRegion(resource, offset, slot, 0, payload.Width);
      return;
    }
    D3D12_TEXTURE_COPY_LOCATION texture = {};
    texture.pResource = resource;
    texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION placed = {};
    placed.pResource = slot;
    placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    placed.PlacedFootprint = layout;
    if (into_slot)
      list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
    else
      list->CopyTextureRegion(&texture, 0, 0, 0, &placed, nullptr);
  }

  bool enqueueExternalWait(End end, const CrossAdapterSyncPoint &point)
  {
    if (!point.fence)
      return true;
    return sameDevice(point.fence, sides[end].device.Get()) && SUCCEEDED(sides[end].queue->Wait(point.fence, point.value));
  }

public:
  CrossAdapterChannel() = default;
  CrossAdapterChannel(const CrossAdapterChannel &) = delete;
  CrossAdapterChannel &operator=(const CrossAdapterChannel &) = delete;
  ~CrossAdapterChannel() { reset(); }

  /**
   * @brief Creates the shared slots, the copy queues and the fences.
   * @param producer Device that sends.
   * @param consumer Device that receives, on another adapter.
   * @param heap_owner producer or consumer: the device that creates the shared heap.
   * @param payload_desc Description of the endpoints: a 2D texture (single plane, no MSAA; subresource 0 is moved) or a
   *        buffer (Width bytes are moved).
   * @param slot_count Frames in flight, from 2 to MAX_SLOTS.
   * @param name Debug name prefix for the queues and slots, may be nullptr.
   * @return S_OK, E_INVALIDARG for unsupported input, DXGI_ERROR_UNSUPPORTED if the adapters disagree on the copy
   *         layout, or the failing HRESULT. On failure the object stays empty.
   */
  HRESULT init(ID3D12Device *producer, ID3D12Device *consumer, ID3D12Device *heap_owner,
    const D3D12_RESOURCE_DESC &payload_desc, uint32_t slot_count, const wchar_t *name)
  {
    reset();
    if (!producer || !consumer || producer == consumer || (heap_owner != producer && heap_owner != consumer) ||
        slot_count < 2 || slot_count > MAX_SLOTS || !isSupportedPayload(producer, payload_desc) || !isSupportedPayload(consumer, payload_desc))
      return E_INVALIDARG;

    uint64_t payloadSize = payload_desc.Width;
    if (isTexture(payload_desc))
    {
      // Both adapters must read the slot with the same layout the other one wrote.
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[END_COUNT] = {};
      uint64_t totals[END_COUNT] = {};
      producer->GetCopyableFootprints(&payload_desc, 0, 1, 0, &layouts[PRODUCER], nullptr, nullptr, &totals[PRODUCER]);
      consumer->GetCopyableFootprints(&payload_desc, 0, 1, 0, &layouts[CONSUMER], nullptr, nullptr, &totals[CONSUMER]);
      if (totals[PRODUCER] == UINT64_MAX || totals[PRODUCER] != totals[CONSUMER] ||
          layouts[PRODUCER].Footprint.RowPitch != layouts[CONSUMER].Footprint.RowPitch)
        return DXGI_ERROR_UNSUPPORTED;
      footprint = layouts[PRODUCER];
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
      return E_INVALIDARG;
    const uint64_t alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    const uint64_t slotStride = (allocation.SizeInBytes + alignment - 1) / alignment * alignment;

    sides[PRODUCER].device = producer;
    sides[CONSUMER].device = consumer;
    slotCount = slot_count;
    slotSize = payloadSize;
    payload = payload_desc;

    const End owner = heap_owner == producer ? PRODUCER : CONSUMER;
    const End other = owner == PRODUCER ? CONSUMER : PRODUCER;
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = slotStride * slot_count;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    HRESULT hr = heap_owner->CreateHeap(&heapDesc, IID_PPV_ARGS(&sides[owner].heap));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr))
      hr = heap_owner->CreateSharedHandle(sides[owner].heap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(hr))
    {
      hr = sides[other].device->OpenSharedHandle(handle, IID_PPV_ARGS(&sides[other].heap));
      CloseHandle(handle);
    }
    if (SUCCEEDED(hr))
      hr = initSide(PRODUCER, slotDesc, slotStride, name);
    if (SUCCEEDED(hr))
      hr = initSide(CONSUMER, slotDesc, slotStride, name);
    if (SUCCEEDED(hr))
      hr = written.init(producer, consumer);
    if (SUCCEEDED(hr))
      hr = consumed.init(consumer, producer);
    if (SUCCEEDED(hr) && name)
      sides[owner].heap->SetName(name);
    if (FAILED(hr))
      reset();
    return hr;
  }

  /**
   * @brief Waits on the CPU until both GPUs are done with the channel, then releases everything.
   */
  void reset()
  {
    if (written)
      written.waitOnCpu(written.issuedValue(), INFINITE);
    if (consumed)
      consumed.waitOnCpu(consumed.issuedValue(), INFINITE);
    for (Side &side : sides)
      side = {};
    written.reset();
    consumed.reset();
    for (Slot &slot : slotStates)
      slot = {};
    slotCount = 0;
    payload = {};
    footprint = {};
    slotSize = 0;
    lastSent = 0;
    lastReceived = 0;
  }

  explicit operator bool() const { return static_cast<bool>(written); }

  /**
   * @brief False if send() would drop the frame because the consumer is late. Has no side effects, so a caller can
   *        skip preparing a source that would not be sent.
   */
  bool isNextSlotFree() const
  {
    if (!*this)
      return false;
    const Slot &slot = slotStates[(lastSent + 1) % slotCount];
    return slot.ticket == 0 || slot.consumedValue != 0 || lastReceived >= slot.ticket;
  }

  /**
   * @brief True if receive() can still copy ticket, i.e. it was sent and its slot was not reused.
   */
  bool holds(uint64_t ticket) const { return *this && ticket != 0 && slotStates[ticket % slotCount].ticket == ticket; }

  /**
   * @brief Copies source on the producer into the next slot.
   * @param source Producer resource matching the payload, in COMMON when the copy starts.
   * @param source_ready Producer fence the copy waits for, e.g. the end of the pass that writes source; may be empty.
   * @param source_offset Byte offset of the payload in a buffer source; 0 for textures.
   * @return Ticket for receive() and the point after which source may be written again; ticket 0 if the input is
   *         invalid, the consumer is late (the frame is dropped) or submission failed.
   */
  Send send(ID3D12Resource *source, CrossAdapterSyncPoint source_ready, uint64_t source_offset = 0)
  {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    if (!*this || !matchesPayload(source, source_offset, PRODUCER, layout) ||
        (source_ready.fence && !sameDevice(source_ready.fence, sides[PRODUCER].device.Get())))
      return {};
    if (!isNextSlotFree())
      return {};
    const uint64_t ticket = lastSent + 1;
    Slot &slot = slotStates[ticket % slotCount];

    Recording *recording = nullptr;
    ID3D12GraphicsCommandList *list = beginRecording(PRODUCER, recording);
    if (!list)
      return {};
    recordCopy(list, source, source_offset, layout, sides[PRODUCER].slots[ticket % slotCount].Get(), true);
    if (!enqueueExternalWait(PRODUCER, source_ready) ||
        (slot.consumedValue != 0 && !consumed.enqueueWait(sides[PRODUCER].queue.Get(), slot.consumedValue)))
    {
      list->Close();
      return {};
    }
    const uint64_t value = submit(PRODUCER, *recording, written);
    if (!value)
    {
      slot = {}; // the copy may have overwritten the frame that was there
      return {};
    }

    slot = {ticket, value, 0};
    lastSent = ticket;
    return {ticket, {written.getFence(CrossAdapterFence::Side::SIGNALER), value}};
  }

  /**
   * @brief Copies the slot holding ticket into destination on the consumer.
   * @param ticket Value returned by send(); it can be received more than once until its slot is reused.
   * @param destination Consumer resource matching the payload, in COMMON when the copy starts.
   * @param destination_free Consumer fence the copy waits for, e.g. the last pass reading destination; may be empty.
   * @param destination_offset Byte offset of the payload in a buffer destination; 0 for textures.
   * @return Point on the consumer after which destination holds the data; empty if the ticket is unknown or overwritten,
   *         the input is invalid or submission failed.
   */
  CrossAdapterSyncPoint receive(uint64_t ticket, ID3D12Resource *destination, CrossAdapterSyncPoint destination_free,
    uint64_t destination_offset = 0)
  {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    if (!*this || ticket == 0 || !matchesPayload(destination, destination_offset, CONSUMER, layout) ||
        (destination_free.fence && !sameDevice(destination_free.fence, sides[CONSUMER].device.Get())))
      return {};
    if (!holds(ticket))
      return {};
    Slot &slot = slotStates[ticket % slotCount];

    Recording *recording = nullptr;
    ID3D12GraphicsCommandList *list = beginRecording(CONSUMER, recording);
    if (!list)
      return {};
    recordCopy(list, destination, destination_offset, layout, sides[CONSUMER].slots[ticket % slotCount].Get(), false);
    if (!enqueueExternalWait(CONSUMER, destination_free) || !written.enqueueWait(sides[CONSUMER].queue.Get(), slot.writtenValue))
    {
      list->Close();
      return {};
    }
    const uint64_t value = submit(CONSUMER, *recording, consumed);
    if (!value)
      return {};

    slot.consumedValue = value;
    if (ticket > lastReceived)
      lastReceived = ticket;
    return {consumed.getFence(CrossAdapterFence::Side::SIGNALER), value};
  }

  /**
   * @brief True once either device is removed. Waits stop holding the queues then, so the data is no longer valid.
   */
  bool isLost() const { return written.isSignalerLost() || consumed.isSignalerLost(); }
};
} // namespace drv3d_dx12
