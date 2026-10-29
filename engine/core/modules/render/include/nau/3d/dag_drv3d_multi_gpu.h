// dag_drv3d_multi_gpu.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
// Shared transfer channels between the primary engine device and a secondary device.
#pragma once

#include <cstdint>

#include "nau/3d/dag_drv3d.h"

class BaseTexture;

namespace d3d::mgpu
{
    /**
     * @brief Direction in which a transfer channel moves data.
     */
    enum class TransferDirection : uint8_t
    {
        primaryToSecondary,
        secondaryToPrimary
    };

    /**
     * @brief Description of a channel that repeatedly moves one 2D image between the GPUs.
     *
     * Both endpoints must have exactly this size and format. Only mip 0 of array slice 0 is moved.
     * Engine textures need TEXCF_RTARGET, TEXCF_UNORDERED or TEXCF_UPDATE_DESTINATION, the same as for BaseTexture::update().
     * Depth is supported as TEXFMT_DEPTH32 (plane 0 only); combined depth-stencil formats are rejected,
     * copy such data into a TEXFMT_R32F texture first.
     */
    struct TransferChannelDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;  ///< TEXFMT_* value shared by both endpoints.
        TransferDirection direction = TransferDirection::secondaryToPrimary;
        uint32_t slotCount = 2;      ///< Shared copies in flight; at least 2, so the producer can write frame N+1 while frame N is read.
        const char* name = nullptr;  ///< Debug name of the shared resources.
    };

    struct TransferChannel;
    using TransferChannelHandle = TransferChannel*;

    /**
     * @brief Identifies one transfer through a channel. Monotonic per channel; 0 means nothing was sent.
     */
    using TransferTicket = uint64_t;

    /**
     * @brief GPU-side synchronization point on the secondary GPU: a native ID3D12Fence * and a value.
     *
     * Passed to the technique so that its own secondary queues can wait for or be waited on by a transfer.
     * The fence is owned by whoever returned it and stays valid until the channel is destroyed.
     */
    struct SecondarySyncPoint
    {
        void* nativeFence = nullptr;
        uint64_t value = 0;
    };

    /**
     * @brief Result of send_from_secondary().
     */
    struct SecondarySend
    {
        TransferTicket ticket = 0;          ///< 0 if nothing was sent.
        SecondarySyncPoint sourceReleased;  ///< After this point the technique may write native_source again.
    };

    /** @brief Borrowed primary-device fence, valid until its transfer channel is destroyed. */
    struct PrimarySyncPoint
    {
        void* nativeFence = nullptr;
        uint64_t value = 0;
    };

    struct PrimarySend
    {
        TransferTicket ticket = 0;
        PrimarySyncPoint sourceReleased;
    };

    /**
     * @brief Sends a persistent texture without waiting on the primary graphics queue.
     * Keep the source alive and unchanged until sourceReleased completes, including after a partial job failure.
     * Call is_primary_transfer_complete() before reusing it. A zero ticket means no copy was submitted.
     */
    NAU_RENDER_EXPORT PrimarySend send_from_primary_deferred(TransferChannelHandle channel, BaseTexture* source);

    /**
     * @brief Starts a receive without waiting on the primary graphics queue.
     * Keep destination alive and unused until the returned point completes. A null fence means failure.
     * The caller must poll before publishing the destination to the engine or reusing the channel.
     */
    NAU_RENDER_EXPORT PrimarySyncPoint begin_receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, BaseTexture* destination);

    /** @brief Polls a primary copy fence. Never waits; device removal is reported as incomplete. */
    NAU_RENDER_EXPORT bool is_primary_transfer_complete(PrimarySyncPoint point);

    // Slot reuse: ticket t occupies slot t % slotCount. A send waits on the GPU only for receives that were already
    // issued for the ticket in that slot. If the consumer skipped that ticket and already received a newer one, the slot
    // is reused without waiting. If the consumer has not received that ticket or a newer one yet, the send is dropped and
    // returns ticket 0 instead of waiting, so a consumer that skips frames can never hang the producer GPU. A send or
    // receive also fails instead of blocking when the CPU has run several submissions ahead of the GPUs.

    /**
     * @brief Creates a transfer channel.
     * @param desc Channel description.
     * @return nullptr if there is no secondary GPU, the format or size is unsupported or a shared resource could not be
     *         created.
     *         The caller then runs its single-GPU path; nothing else needs to be released.
     *
     * Call on the render thread while the driver is initialized. All channels must be destroyed before driver shutdown
     * and before device recovery; after recovery create them again.
     */
    NAU_RENDER_EXPORT TransferChannelHandle create_transfer_channel(const TransferChannelDesc& desc);

    /**
     * @brief Destroys a channel. Waits on the CPU until the GPUs no longer use its shared resources. Accepts nullptr.
     */
    NAU_RENDER_EXPORT void destroy_transfer_channel(TransferChannelHandle channel);

    /**
     * @brief Checks whether a format can be moved by a transfer channel on the current pair of GPUs.
     */
    NAU_RENDER_EXPORT bool is_transfer_format_supported(uint32_t format);

    /**
     * @brief Polls whether a ticket's source copy has finished. Does not record commands or wait on either GPU.
     * False for unknown/overwritten tickets and removed devices. A subsequent receive still performs its local copy.
     */
    NAU_RENDER_EXPORT bool is_transfer_ready(TransferChannelHandle channel, TransferTicket ticket);

    /**
     * @brief SecondaryToPrimary: copies a native texture on GPU 1 into the next free slot.
     * @param channel Channel created with TransferDirection::secondaryToPrimary.
     * @param native_source ID3D12Resource * on GPU 1 matching the channel's payload, in D3D12_RESOURCE_STATE_COMMON
     *        when the copy starts. It decays back to COMMON when the copy ends.
     * @param source_ready The technique's own fence/value that is signaled once source is fully written.
     * @return Ticket for begin_receive_on_primary() and the point after which source may be rewritten; ticket 0 on failure.
     *
     * Executed on the secondary copy queue after source_ready, and after GPU 0 has finished reading the slot.
     * Never blocks the CPU.
     */
    NAU_RENDER_EXPORT SecondarySend send_from_secondary(TransferChannelHandle channel, void* native_source, SecondarySyncPoint source_ready);

    /**
     * @brief PrimaryToSecondary: copies the data of a ticket into a native texture on GPU 1.
     * @param channel Channel created with TransferDirection::primaryToSecondary.
     * @param ticket Value returned by send_from_primary_deferred().
     * @param native_destination ID3D12Resource * on GPU 1 matching the channel's payload, in D3D12_RESOURCE_STATE_COMMON
     *        and not used by the technique until the returned point is reached.
     * @param destination_free Last use of the destination on GPU 1. The copy queue waits for it before overwriting;
     *        this is required when reusing an input texture that a previous render submission may still read.
     * @return Point that the technique's queues on GPU 1 must wait for before reading destination;
     *         {nullptr, 0} on failure.
     */
    NAU_RENDER_EXPORT SecondarySyncPoint receive_on_secondary(TransferChannelHandle channel, TransferTicket ticket, void* native_destination, SecondarySyncPoint destination_free = {});
}  // namespace d3d::mgpu
