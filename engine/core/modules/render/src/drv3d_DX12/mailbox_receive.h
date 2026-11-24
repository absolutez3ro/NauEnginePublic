// mailbox_receive.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include "nau/directx/d3d12.h"

namespace drv3d_dx12
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
        ID3D12Resource* ao[MAILBOX_OUT_SLOTS] = {};
        ID3D12Resource* id[MAILBOX_OUT_SLOTS] = {};
        uint32_t copyOrder[MAILBOX_OUT_SLOTS] = {};  ///< Ascending source-frame order so the newer result wins if two entries are visible.
        uint32_t slotCount = MAILBOX_OUT_SLOTS;
        bool gpuSelection = true;
        int32_t cpuSlot = -1;
        MailboxReceiveDesc()
        {
            for (uint32_t i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                copyOrder[i] = i;
            }
        }
    };
}
