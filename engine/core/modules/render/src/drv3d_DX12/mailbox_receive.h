// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    constexpr uint32_t MAILBOX_IN_SLOTS = 5;
    // Six slots caused output-pressure skips with Nau's three-frame CPU lead; eight retain readers without stalling.
    constexpr uint32_t MAILBOX_OUT_SLOTS = 8;

    // Borrowed resources remain alive until the primary graphics stream has completed.
    // Native resources are COMMON between calls, except readback, which stays COPY_DEST.
    struct MailboxReceiveDesc
    {
        ID3D12Resource* mailbox = nullptr;
        ID3D12Resource* predicate = nullptr;
        ID3D12Resource* frameId = nullptr;
        ID3D12Resource* readback = nullptr;
        uint64_t readbackOffset = 0;
        ID3D12Resource* ao[MAILBOX_OUT_SLOTS] = {};
        ID3D12Resource* id[MAILBOX_OUT_SLOTS] = {};
        // Ascending source-frame order at recording time: a torn two-entry mailbox then leaves the newer AO last.
        uint32_t copyOrder[MAILBOX_OUT_SLOTS] = {};
        uint32_t slotCount = MAILBOX_OUT_SLOTS;
        bool gpuSelection = true;
        int32_t cpuSlot = -1;
        MailboxReceiveDesc()
        {
            for (uint32_t i = 0; i < MAILBOX_OUT_SLOTS; ++i) copyOrder[i] = i;
        }
    };
}
