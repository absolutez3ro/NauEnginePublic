// Copyright 2024 N-GINN LLC. All rights reserved.
// Moving data between two GPUs (multi-GPU contract). Declarations only: the DX12 implementation follows separately.
#pragma once

#include <cstdint>

#include "nau/3d/dag_drv3d.h" // d3d::GpuId, d3d::has_secondary_gpu(), NAU_RENDER_EXPORT (MG-06)

class BaseTexture;
class Sbuffer;

namespace d3d::mgpu
{
/**
 * @brief Direction in which a transfer channel moves data.
 */
enum class TransferDirection : uint8_t
{
    PrimaryToSecondary,
    SecondaryToPrimary
};

/**
 * @brief What a transfer channel moves.
 */
enum class TransferPayload : uint8_t
{
    Texture2D, ///< One 2D image: width, height and format describe it.
    Buffer ///< A raw buffer of byteSize bytes: vertices, indices, constants, matrices.
};

/**
 * @brief Description of a channel that repeatedly moves one 2D image or one buffer between the GPUs.
 *
 * Texture2D: both endpoints must have exactly this size and format. Only mip 0 of array slice 0 is moved.
 * Depth is supported as TEXFMT_DEPTH32 (plane 0 only); combined depth-stencil formats are rejected,
 * copy such data into a TEXFMT_R32F texture first.
 *
 * Buffer: both endpoints must be at least byteSize bytes long; bytes [0, byteSize) are moved, width, height and format
 * are ignored.
 */
struct TransferChannelDesc
{
    TransferPayload payload = TransferPayload::Texture2D;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; ///< TEXFMT_* value shared by both endpoints.
    uint32_t byteSize = 0; ///< Buffer payload only.
    TransferDirection direction = TransferDirection::SecondaryToPrimary;
    uint32_t slotCount = 2; ///< Shared copies in flight; at least 2, so the producer can write frame N+1 while frame N is read.
    const char *name = nullptr; ///< Debug name of the shared resources.
};

struct TransferChannel;
using TransferChannelHandle = TransferChannel *;

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
    void *nativeFence = nullptr;
    uint64_t value = 0;
};

/**
 * @brief Result of send_from_secondary().
 */
struct SecondarySend
{
    TransferTicket ticket = 0; ///< 0 if nothing was sent.
    SecondarySyncPoint sourceReleased; ///< After this point the technique may write native_source again.
};

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
NAU_RENDER_EXPORT TransferChannelHandle create_transfer_channel(const TransferChannelDesc &desc);

/**
 * @brief Destroys a channel. Waits on the CPU until the GPUs no longer use its shared resources. Accepts nullptr.
 */
NAU_RENDER_EXPORT void destroy_transfer_channel(TransferChannelHandle channel);

/**
 * @brief Checks whether a format can be moved by a transfer channel on the current pair of GPUs.
 */
NAU_RENDER_EXPORT bool is_transfer_format_supported(uint32_t format);

/**
 * @brief PrimaryToSecondary: copies an engine texture on GPU 0 into the next free slot.
 * @param channel Channel created with TransferDirection::PrimaryToSecondary.
 * @param source Engine texture on GPU 0 with the channel's size and format; the driver handles its states.
 * @return Ticket for receive_on_secondary(), or 0 on failure or when the frame was dropped (see slot reuse above).
 *
 * Recorded into the driver's own command stream after all previously issued work that writes source.
 * Never blocks the CPU: if the slot is still being read by GPU 1, GPU 0 waits for it on the GPU.
 */
NAU_RENDER_EXPORT TransferTicket send_from_primary(TransferChannelHandle channel, BaseTexture *source);

/**
 * @brief PrimaryToSecondary, Buffer payload: same as the texture overload for an engine buffer on GPU 0.
 */
NAU_RENDER_EXPORT TransferTicket send_from_primary(TransferChannelHandle channel, Sbuffer *source);

/**
 * @brief SecondaryToPrimary: makes the data of a ticket visible in an engine texture on GPU 0.
 * @param channel Channel created with TransferDirection::SecondaryToPrimary.
 * @param ticket Value returned by send_from_secondary().
 * @param destination Engine texture on GPU 0 with the channel's size and format; the driver handles its states.
 * @return false if the ticket is unknown or already overwritten; destination is left unchanged.
 *
 * GPU 0 waits for the ticket on the GPU; work issued after this call sees the new contents. Never blocks the CPU.
 */
NAU_RENDER_EXPORT bool receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, BaseTexture *destination);

/**
 * @brief SecondaryToPrimary, Buffer payload: same as the texture overload for an engine buffer on GPU 0.
 */
NAU_RENDER_EXPORT bool receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, Sbuffer *destination);

/**
 * @brief SecondaryToPrimary: copies a native texture or buffer on GPU 1 into the next free slot.
 * @param channel Channel created with TransferDirection::SecondaryToPrimary.
 * @param native_source ID3D12Resource * on GPU 1 matching the channel's payload, in D3D12_RESOURCE_STATE_COMMON
 *        when the copy starts. It decays back to COMMON when the copy ends.
 * @param source_ready The technique's own fence/value that is signaled once source is fully written.
 * @return Ticket for receive_on_primary() and the point after which source may be rewritten; ticket 0 on failure.
 *
 * Executed on the secondary copy queue after source_ready, and after GPU 0 has finished reading the slot.
 * Never blocks the CPU.
 */
NAU_RENDER_EXPORT SecondarySend send_from_secondary(TransferChannelHandle channel, void *native_source,
    SecondarySyncPoint source_ready);

/**
 * @brief PrimaryToSecondary: copies the data of a ticket into a native texture or buffer on GPU 1.
 * @param channel Channel created with TransferDirection::PrimaryToSecondary.
 * @param ticket Value returned by send_from_primary().
 * @param native_destination ID3D12Resource * on GPU 1 matching the channel's payload, in D3D12_RESOURCE_STATE_COMMON
 *        and not used by the technique until the returned point is reached.
 * @return Point that the technique's queues on GPU 1 must wait for before reading destination;
 *         {nullptr, 0} on failure.
 */
NAU_RENDER_EXPORT SecondarySyncPoint receive_on_secondary(TransferChannelHandle channel, TransferTicket ticket,
    void *native_destination);
} // namespace d3d::mgpu
