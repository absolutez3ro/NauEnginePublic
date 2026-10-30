// Copyright 2024 N-GINN LLC. All rights reserved.
// d3d::mgpu transfer channels on top of CrossAdapterChannel: engine textures on GPU 0, native resources on
// GPU 1.

#include "multi_gpu_transfer.h"

#include <EASTL/unique_ptr.h>

#include "device.h"
#include "format_store.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "texture.h"

using drv3d_dx12::CrossAdapterChannel;
using drv3d_dx12::DeviceContext;
using drv3d_dx12::FormatStore;
using drv3d_dx12::getbasetex;
using drv3d_dx12::Image;

struct d3d::mgpu::TransferChannel
{
    CrossAdapterChannel channel;
    TransferDirection direction = TransferDirection::secondaryToPrimary;
    // Signaled on the engine graphics queue once the engine is done with an endpoint; the channel's copy queue on GPU 0
    // waits for it. Same device, so an ordinary fence.
    ComPtr<ID3D12Fence> engineFence;
    uint64_t engineFenceValue = 0;
    bool synchronization = true;
};

drv3d_dx12::SharedTextureRead drv3d_dx12::getSecondarySharedInput(d3d::mgpu::TransferChannel* channel, uint64_t ticket, ID3D12Resource* destination)
{
    if (!channel || channel->direction != d3d::mgpu::TransferDirection::primaryToSecondary || channel->synchronization)
    {
        return {};
    }
    return channel->channel.getSharedRead(ticket, destination);
}

namespace
{
    ID3D12Device* native_device(d3d::GpuId gpu)
    {
        return static_cast<ID3D12Device*>(d3d::get_device(gpu));
    }

    DXGI_FORMAT dxgi_texture_format(uint32_t format)
    {
        return FormatStore::fromCreateFlags(format).asDxGiTextureCreateFormat();
    }

    bool is_single_plane(ID3D12Device* device, DXGI_FORMAT format)
    {
        D3D12_FEATURE_DATA_FORMAT_INFO info = {format, 0};
        return format != DXGI_FORMAT_UNKNOWN && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) &&
               info.PlaneCount == 1;
    }

    D3D12_RESOURCE_DESC payload_desc(const d3d::mgpu::TransferChannelDesc& desc)
    {
        D3D12_RESOURCE_DESC result = {};
        result.Height = 1;
        result.DepthOrArraySize = 1;
        result.MipLevels = 1;
        result.SampleDesc.Count = 1;

        result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        result.Width = desc.width;
        result.Height = desc.height;
        result.Format = dxgi_texture_format(desc.format);
        return result;
    }

    Image* engine_image(BaseTexture* texture)
    {
        Image* image = texture ? getbasetex(texture)->getDeviceImage() : nullptr;
        if (!image || !image->getHandle())
        {
            return nullptr;
        }
        // Static textures stay in their read state and have no tracked state to hand over to a copy queue.
        if (!image->hasTrackedState())
        {
            NAU_LOG_ERROR(
                "DX12: multi-GPU transfer of texture <{}> that does not support it, the texture needs either "
                "TEXCF_UPDATE_DESTINATION, TEXCF_RTARGET or TEXCF_UNORDERED create flags specified",
                texture->getResName());
            return nullptr;
        }
        return image;
    }

    bool accepts(const d3d::mgpu::TransferChannel* channel, d3d::mgpu::TransferDirection direction)
    {
        return channel && channel->direction == direction;
    }

    d3d::mgpu::PrimarySend send_engine_resource(d3d::mgpu::TransferChannel* channel, Image* image, ID3D12Resource* resource)
    {
        // A late consumer drops the frame; find out before flushing engine work for nothing.
        if (!channel->channel.isNextSlotFree())
        {
            return {};
        }
        DeviceContext& ctx = drv3d_dx12::get_device().getContext();
        const uint64_t engineDone = ++channel->engineFenceValue;
        ctx.releaseToForeignQueue(image, channel->engineFence.Get(), engineDone);
        const auto sent = channel->channel.send(resource, {channel->engineFence.Get(), engineDone});
        if (sent.ticket == 0)
        {
            return {};
        }

        return {
            sent.ticket,
            {sent.sourceReleased.fence, sent.sourceReleased.value}
        };
    }

    d3d::mgpu::PrimarySyncPoint receive_engine_resource(d3d::mgpu::TransferChannel* channel, d3d::mgpu::TransferTicket ticket, Image* image, ID3D12Resource* resource)
    {
        if (!channel->channel.holds(ticket))
        {
            return {};
        }
        DeviceContext& ctx = drv3d_dx12::get_device().getContext();
        const uint64_t engineDone = ++channel->engineFenceValue;
        ctx.releaseToForeignQueue(image, channel->engineFence.Get(), engineDone);
        const auto ready = channel->channel.receive(ticket, resource, {channel->engineFence.Get(), engineDone});
        if (!ready.fence)
        {
            return {};
        }

        return {ready.fence, ready.value};
    }
}  // namespace

d3d::mgpu::TransferChannelHandle d3d::mgpu::create_transfer_channel(const TransferChannelDesc& desc)
{
    if (!d3d::is_inited() || !d3d::has_secondary_gpu())
    {
        return nullptr;
    }
    if (desc.slotCount < (desc.synchronization ? 2u : 1u) || desc.slotCount > CrossAdapterChannel::MAX_SLOTS || (!desc.synchronization && desc.slotCount != 1))
    {
        NAU_LOG_ERROR("DX12: multi-GPU channel '{}' asks for {} slots, invalid synchronization/slot count, maximum {}", desc.name ? desc.name : "", desc.slotCount,
                      CrossAdapterChannel::MAX_SLOTS);
        return nullptr;
    }
    if (!is_transfer_format_supported(desc.format))
    {
        NAU_LOG_ERROR("DX12: multi-GPU channel '{}' uses an unsupported format {:#x}", desc.name ? desc.name : "", desc.format);
        return nullptr;
    }

    ID3D12Device* primary = native_device(PRIMARY_GPU);
    ID3D12Device* secondary = native_device(SECONDARY_GPU);
    const bool fromPrimary = desc.direction == TransferDirection::primaryToSecondary;

    auto result = eastl::make_unique<TransferChannel>();
    result->direction = desc.direction;
    result->synchronization = desc.synchronization;
    HRESULT hr = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->engineFence));
    if (SUCCEEDED(hr))
    {
        wchar_t name[64] = {};
        if (desc.name)
        {
            swprintf(name, 64, L"%hs", desc.name);
        }
        // Create the shared heap on GPU 0 so its ownership follows the engine device. Transfer cost is adapter-dependent.
        hr = result->channel.init(fromPrimary ? primary : secondary, fromPrimary ? secondary : primary, primary, payload_desc(desc),
                                  desc.slotCount, desc.name ? name : nullptr, desc.synchronization);
    }
    if (FAILED(hr))
    {
        NAU_LOG_ERROR("DX12: multi-GPU channel '{}' could not be created, HRESULT {:#x}", desc.name ? desc.name : "",
                      static_cast<uint32_t>(hr));
        return nullptr;
    }
    return result.release();
}

void d3d::mgpu::destroy_transfer_channel(TransferChannelHandle channel)
{
    if (!channel)
    {
        return;
    }
    // Engine commands that signal or wait for this channel's fences may still sit in the command stream: let the engine
    // finish them before the fences go away. The channel then waits for its own copy queues.
    drv3d_dx12::get_device().getContext().wait();
    delete channel;
}

bool d3d::mgpu::is_transfer_format_supported(uint32_t format)
{
    if (!d3d::is_inited() || !d3d::has_secondary_gpu())
    {
        return false;
    }
    const DXGI_FORMAT dxgiFormat = dxgi_texture_format(format);
    return is_single_plane(native_device(PRIMARY_GPU), dxgiFormat) && is_single_plane(native_device(SECONDARY_GPU), dxgiFormat);
}

bool d3d::mgpu::is_transfer_ready(TransferChannelHandle channel, TransferTicket ticket)
{
    return channel && channel->channel.isReady(ticket);
}

d3d::mgpu::PrimarySend d3d::mgpu::send_from_primary_deferred(TransferChannelHandle channel, BaseTexture* source)
{
    Image* image = engine_image(source);
    if (!accepts(channel, TransferDirection::primaryToSecondary) || !image)
    {
        return {};
    }
    return send_engine_resource(channel, image, image->getHandle());
}

d3d::mgpu::PrimarySyncPoint d3d::mgpu::begin_receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, BaseTexture* destination)
{
    Image* image = engine_image(destination);
    if (!accepts(channel, TransferDirection::secondaryToPrimary) || !image)
    {
        return {};
    }
    return receive_engine_resource(channel, ticket, image, image->getHandle());
}

bool d3d::mgpu::receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, BaseTexture* destination)
{
    if (!channel || channel->synchronization)
    {
        return false;
    }
    const PrimarySyncPoint ready = begin_receive_on_primary(channel, ticket, destination);
    if (!ready.nativeFence)
    {
        return false;
    }
    drv3d_dx12::get_device().getContext().waitForForeignQueue(static_cast<ID3D12Fence*>(ready.nativeFence), ready.value);
    return true;
}

bool d3d::mgpu::is_primary_transfer_complete(PrimarySyncPoint point)
{
    if (!point.nativeFence)
    {
        return point.value == 0;
    }
    const uint64_t completed = static_cast<ID3D12Fence*>(point.nativeFence)->GetCompletedValue();
    return completed != UINT64_MAX && completed >= point.value;
}

d3d::mgpu::SecondarySend d3d::mgpu::send_from_secondary(TransferChannelHandle channel, void* native_source, SecondarySyncPoint source_ready)
{
    if (!channel || channel->direction != TransferDirection::secondaryToPrimary)
    {
        return {};
    }
    const auto sent = channel->channel.send(static_cast<ID3D12Resource*>(native_source),
                                            {static_cast<ID3D12Fence*>(source_ready.nativeFence), source_ready.value});
    return {
        sent.ticket,
        {sent.sourceReleased.fence, sent.sourceReleased.value}
    };
}

d3d::mgpu::SecondarySyncPoint d3d::mgpu::receive_on_secondary(TransferChannelHandle channel, TransferTicket ticket, void* native_destination, SecondarySyncPoint destination_free)
{
    if (!channel || channel->direction != TransferDirection::primaryToSecondary)
    {
        return {};
    }
    const drv3d_dx12::CrossAdapterSyncPoint ready = channel->channel.receive(ticket, static_cast<ID3D12Resource*>(native_destination),
                                                                             {static_cast<ID3D12Fence*>(destination_free.nativeFence), destination_free.value});
    return {ready.fence, ready.value};
}
