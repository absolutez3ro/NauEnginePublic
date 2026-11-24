// mailbox_receive.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace nau::mgpu
{
    constexpr uint32_t MAILBOX_IN_SLOTS = 5;
    // Outputs cover the pinned latest result and receives already recorded by Nau's CPU frontend.
    constexpr uint32_t MAILBOX_OUT_SLOTS = 8;

    /**
     * @brief Records a primary-side mailbox receive using borrowed resources.
     * All resources must stay alive until the primary graphics commands finish.
     * Native resources remain in COMMON between calls, except readback in COPY_DEST.
     */
    struct MailboxReceiveDesc
    {
        ID3D12Resource* mailbox = nullptr;
        ID3D12Resource* predicate = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* readback = nullptr;
        uint64_t readbackOffset = 0;
        ID3D12Resource* payload[MAILBOX_OUT_SLOTS] = {};
        ID3D12Resource* id[MAILBOX_OUT_SLOTS] = {};
        uint32_t copyOrder[MAILBOX_OUT_SLOTS] = {};  ///< Ascending source-frame order so the newer result wins if two entries are visible.
        uint32_t slotCount = MAILBOX_OUT_SLOTS;
        MailboxReceiveDesc()
        {
            for (uint32_t i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                copyOrder[i] = i;
            }
        }
    };
    /**
     * @brief Copies the latest visible payload and its source ID without waiting for the producer.
     * @pre Destination is COPY_DEST. Native mailbox resources are COMMON, readback is COPY_DEST.
     * The caller must retain resources and retire output slots after its queue completes.
     */
    inline void recordMailboxReceive(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, const MailboxReceiveDesc& desc)
    {
        const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            // The native textures have one mip and slice. Subresource zero also satisfies Nau's barrier validator.
            barrier.Transition = {resource, 0, before, after};
            list->ResourceBarrier(1, &barrier);
        };
        transition(desc.mailbox, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(desc.predicate, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(desc.predicate, 0, desc.mailbox, 0, sizeof(uint64_t) * desc.slotCount);
        transition(desc.mailbox, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(desc.predicate, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);

        transition(desc.frameId, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (uint32_t order = 0; order < desc.slotCount; ++order)
        {
            const uint32_t slot = desc.copyOrder[order];
            // SetPredication suppresses operations when the data matches the operation: zero skips,
            // nonzero executes. Barriers are unconditional; AO and its input-frame ID use one predicate.
            list->SetPredication(desc.predicate, sizeof(uint64_t) * slot, D3D12_PREDICATION_OP_EQUAL_ZERO);
            transition(desc.payload[slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(desc.id[slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(destination, desc.payload[slot]);
            list->CopyResource(desc.frameId, desc.id[slot]);
            transition(desc.id[slot], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            transition(desc.payload[slot], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        // Do not leave native predication active after returning to Nau's render graph.
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        transition(desc.frameId, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(desc.predicate, D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COMMON);

        if (desc.readback)
        {
            // Read the ID actually paired with the AO selected by this graphics execution, including a
            // retained previous result. Its caller releases the readback slot only after a later F0 signal.
            transition(desc.frameId, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source = {}, readbackDestination = {};
            source.pResource = desc.frameId;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            readbackDestination.pResource = desc.readback;
            readbackDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            readbackDestination.PlacedFootprint.Offset = desc.readbackOffset;
            readbackDestination.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
            list->CopyTextureRegion(&readbackDestination, 0, 0, 0, &source, nullptr);
            transition(desc.frameId, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
    }

}  // namespace nau::mgpu
