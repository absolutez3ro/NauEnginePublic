// mailbox_job.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstring>

#include "cross_adapter_fence.h"
#include "mailbox_receive.h"

namespace nau::mgpu
{
    /** @brief A reserved mailbox slot and its producer-side dependencies. */
    struct MailboxJob
    {
        struct SyncPoint
        {
            ID3D12Fence* fence = nullptr;
            uint64_t value = 0;
        } inputsReady;
        CrossAdapterFence* completion = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* mailbox = nullptr;
        uint32_t outputSlot = 0;
    };

    struct MailboxCompletion
    {
        uint64_t dataReady = 0;
        uint64_t published = 0;
    };

    /** @brief Records the source ID next to an effect's output. Upload storage must remain immutable until completion. */
    inline void recordMailboxId(ID3D12GraphicsCommandList* list, const MailboxJob& job, uint64_t frame, ID3D12Resource* upload, uint8_t* mapped, uint64_t offset)
    {
        const uint32_t id = static_cast<uint32_t>(frame);
        std::memcpy(mapped + offset, &id, sizeof(id));
        D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
        source.pResource = upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = offset;
        source.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        destination.pResource = job.frameId;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {job.frameId, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1, &barrier);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
    }

    /** @brief Records metadata publication, submitted only after the payload signal. */
    inline void recordMailboxPublication(ID3D12GraphicsCommandList* list, const MailboxJob& job, uint64_t frame, ID3D12Resource* upload, uint8_t* mapped, uint64_t offset)
    {
        uint64_t entries[MAILBOX_OUT_SLOTS] = {};
        entries[job.outputSlot] = frame;
        std::memcpy(mapped + offset, entries, sizeof(entries));
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {job.mailbox, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(job.mailbox, 0, upload, offset, sizeof(entries));
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
    }

    /**
     * @brief Submits payload and metadata in order. Only the secondary queue waits for input readiness.
     * @param submitted Set once GPU work is issued, including partial failure; the caller must retain its recording.
     */
    inline HRESULT submitMailboxJob(ID3D12CommandQueue* queue, ID3D12CommandList* payload, ID3D12CommandList* publication, const MailboxJob& job, MailboxCompletion& completion, bool& submitted)
    {
        completion = {};
        submitted = false;
        const HRESULT result = queue->Wait(job.inputsReady.fence, job.inputsReady.value);
        if (FAILED(result))
        {
            return result;
        }
        queue->ExecuteCommandLists(1, &payload);
        submitted = true;
        completion.dataReady = job.completion->enqueueSignal(queue);
        if (!completion.dataReady)
        {
            return E_FAIL;
        }
        queue->ExecuteCommandLists(1, &publication);
        completion.published = job.completion->enqueueSignal(queue);
        return completion.published ? S_OK : E_FAIL;
    }
}  // namespace nau::mgpu
