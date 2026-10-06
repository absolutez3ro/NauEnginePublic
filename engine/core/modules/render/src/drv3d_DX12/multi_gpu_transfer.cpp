// Copyright 2024 N-GINN LLC. All rights reserved.
// d3d::mgpu transfer channels on top of CrossAdapterChannel: engine textures and buffers on GPU 0, native resources on
// GPU 1.

#include "device.h"
#include "buffer.h"
#include "format_store.h"
#include "multi_gpu_transfer.h"
#include "texture.h"

#include "nau/3d/dag_drv3d_multi_gpu.h"

#include <EASTL/unique_ptr.h>

using namespace drv3d_dx12;

struct d3d::mgpu::TransferChannel
{
  CrossAdapterChannel channel;
  TransferDirection direction = TransferDirection::SecondaryToPrimary;
  TransferPayload payload = TransferPayload::Texture2D;
  // Signaled on the engine graphics queue once the engine is done with an endpoint; the channel's copy queue on GPU 0
  // waits for it. Same device, so an ordinary fence.
  ComPtr<ID3D12Fence> engineFence;
  uint64_t engineFenceValue = 0;
};

namespace
{
ID3D12Device *native_device(d3d::GpuId gpu) { return static_cast<ID3D12Device *>(d3d::get_device(gpu)); }

DXGI_FORMAT dxgi_texture_format(uint32_t format) { return FormatStore::fromCreateFlags(format).asDxGiTextureCreateFormat(); }

bool is_single_plane(ID3D12Device *device, DXGI_FORMAT format)
{
  D3D12_FEATURE_DATA_FORMAT_INFO info = {format, 0};
  return format != DXGI_FORMAT_UNKNOWN && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) &&
         info.PlaneCount == 1;
}

D3D12_RESOURCE_DESC payload_desc(const d3d::mgpu::TransferChannelDesc &desc)
{
  D3D12_RESOURCE_DESC result = {};
  result.Height = 1;
  result.DepthOrArraySize = 1;
  result.MipLevels = 1;
  result.SampleDesc.Count = 1;
  if (desc.payload == d3d::mgpu::TransferPayload::Buffer)
  {
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = desc.byteSize;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
  }
  result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  result.Width = desc.width;
  result.Height = desc.height;
  result.Format = dxgi_texture_format(desc.format);
  return result;
}

// A copy queue can read buffers in default and upload heaps, but write only to default heaps.
bool is_copy_endpoint(ID3D12Resource *resource, bool written)
{
  D3D12_HEAP_PROPERTIES properties = {};
  if (FAILED(resource->GetHeapProperties(&properties, nullptr)))
    return false;
  return properties.Type == D3D12_HEAP_TYPE_DEFAULT || (!written && properties.Type == D3D12_HEAP_TYPE_UPLOAD);
}

Image *engine_image(BaseTexture *texture)
{
  Image *image = texture ? getbasetex(texture)->getDeviceImage() : nullptr;
  return image && image->getHandle() ? image : nullptr;
}

const BufferState *engine_buffer(Sbuffer *buffer, bool written)
{
  if (!buffer)
    return nullptr;
  const BufferState &state = static_cast<GenericBufferInterface *>(buffer)->getDeviceBuffer();
  return state.buffer && is_copy_endpoint(state.buffer, written) ? &state : nullptr;
}

bool accepts(const d3d::mgpu::TransferChannel *channel, d3d::mgpu::TransferDirection direction,
  d3d::mgpu::TransferPayload payload)
{
  return channel && channel->direction == direction && channel->payload == payload;
}

// Common path of both send_from_primary overloads: image is nullptr for buffers.
d3d::mgpu::TransferTicket send_engine_resource(d3d::mgpu::TransferChannel *channel, Image *image, ID3D12Resource *resource,
  uint64_t offset)
{
  // A late consumer drops the frame; find out before flushing engine work for nothing.
  if (!channel->channel.isNextSlotFree())
    return 0;
  DeviceContext &ctx = drv3d_dx12::get_device().getContext();
  const uint64_t engineDone = ++channel->engineFenceValue;
  ctx.releaseToForeignQueue(image, channel->engineFence.Get(), engineDone);
  const auto sent = channel->channel.send(resource, {channel->engineFence.Get(), engineDone}, offset);
  if (sent.ticket == 0)
    return 0;
  // The engine must not overwrite the source before the copy into the slot has read it.
  ctx.waitForForeignQueue(sent.sourceReleased.fence, sent.sourceReleased.value);
  return sent.ticket;
}

// Common path of both receive_on_primary overloads: image is nullptr for buffers.
bool receive_engine_resource(d3d::mgpu::TransferChannel *channel, d3d::mgpu::TransferTicket ticket, Image *image,
  ID3D12Resource *resource, uint64_t offset)
{
  if (!channel->channel.holds(ticket))
    return false;
  DeviceContext &ctx = drv3d_dx12::get_device().getContext();
  const uint64_t engineDone = ++channel->engineFenceValue;
  ctx.releaseToForeignQueue(image, channel->engineFence.Get(), engineDone);
  const auto ready = channel->channel.receive(ticket, resource, {channel->engineFence.Get(), engineDone}, offset);
  if (!ready.fence)
    return false;
  ctx.waitForForeignQueue(ready.fence, ready.value);
  return true;
}
} // namespace

d3d::mgpu::TransferChannelHandle d3d::mgpu::create_transfer_channel(const TransferChannelDesc &desc)
{
  if (!d3d::is_inited() || !d3d::has_secondary_gpu())
    return nullptr;
  if (desc.slotCount < 2 || desc.slotCount > CrossAdapterChannel::MAX_SLOTS)
  {
    NAU_LOG_ERROR("DX12: multi-GPU channel '{}' asks for {} slots, supported 2..{}", desc.name ? desc.name : "", desc.slotCount,
      CrossAdapterChannel::MAX_SLOTS);
    return nullptr;
  }
  if (desc.payload == TransferPayload::Texture2D && !is_transfer_format_supported(desc.format))
  {
    NAU_LOG_ERROR("DX12: multi-GPU channel '{}' uses an unsupported format {:#x}", desc.name ? desc.name : "", desc.format);
    return nullptr;
  }

  ID3D12Device *primary = native_device(PRIMARY_GPU);
  ID3D12Device *secondary = native_device(SECONDARY_GPU);
  const bool fromPrimary = desc.direction == TransferDirection::PrimaryToSecondary;

  auto result = eastl::make_unique<TransferChannel>();
  result->direction = desc.direction;
  result->payload = desc.payload;
  HRESULT hr = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->engineFence));
  if (SUCCEEDED(hr))
  {
    wchar_t name[64] = {};
    if (desc.name)
      swprintf(name, 64, L"%hs", desc.name);
    // The heap lives on GPU 0: its lifetime follows the engine device, and where it lives barely changes the speed.
    hr = result->channel.init(fromPrimary ? primary : secondary, fromPrimary ? secondary : primary, primary, payload_desc(desc),
      desc.slotCount, desc.name ? name : nullptr);
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
    return;
  // Engine commands that signal or wait for this channel's fences may still sit in the command stream: let the engine
  // finish them before the fences go away. The channel then waits for its own copy queues.
  drv3d_dx12::get_device().getContext().wait();
  delete channel;
}

bool d3d::mgpu::is_transfer_format_supported(uint32_t format)
{
  if (!d3d::is_inited() || !d3d::has_secondary_gpu())
    return false;
  const DXGI_FORMAT dxgiFormat = dxgi_texture_format(format);
  return is_single_plane(native_device(PRIMARY_GPU), dxgiFormat) && is_single_plane(native_device(SECONDARY_GPU), dxgiFormat);
}

d3d::mgpu::TransferTicket d3d::mgpu::send_from_primary(TransferChannelHandle channel, BaseTexture *source)
{
  Image *image = engine_image(source);
  if (!accepts(channel, TransferDirection::PrimaryToSecondary, TransferPayload::Texture2D) || !image)
    return 0;
  return send_engine_resource(channel, image, image->getHandle(), 0);
}

d3d::mgpu::TransferTicket d3d::mgpu::send_from_primary(TransferChannelHandle channel, Sbuffer *source)
{
  const BufferState *buffer = engine_buffer(source, false);
  if (!accepts(channel, TransferDirection::PrimaryToSecondary, TransferPayload::Buffer) || !buffer)
    return 0;
  return send_engine_resource(channel, nullptr, buffer->buffer, buffer->currentOffset());
}

bool d3d::mgpu::receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, BaseTexture *destination)
{
  Image *image = engine_image(destination);
  if (!accepts(channel, TransferDirection::SecondaryToPrimary, TransferPayload::Texture2D) || !image)
    return false;
  return receive_engine_resource(channel, ticket, image, image->getHandle(), 0);
}

bool d3d::mgpu::receive_on_primary(TransferChannelHandle channel, TransferTicket ticket, Sbuffer *destination)
{
  const BufferState *buffer = engine_buffer(destination, true);
  if (!accepts(channel, TransferDirection::SecondaryToPrimary, TransferPayload::Buffer) || !buffer)
    return false;
  return receive_engine_resource(channel, ticket, nullptr, buffer->buffer, buffer->currentOffset());
}

d3d::mgpu::SecondarySend d3d::mgpu::send_from_secondary(TransferChannelHandle channel, void *native_source,
  SecondarySyncPoint source_ready)
{
  if (!channel || channel->direction != TransferDirection::SecondaryToPrimary)
    return {};
  const auto sent = channel->channel.send(static_cast<ID3D12Resource *>(native_source),
    {static_cast<ID3D12Fence *>(source_ready.nativeFence), source_ready.value});
  return {sent.ticket, {sent.sourceReleased.fence, sent.sourceReleased.value}};
}

d3d::mgpu::SecondarySyncPoint d3d::mgpu::receive_on_secondary(TransferChannelHandle channel, TransferTicket ticket,
  void *native_destination)
{
  if (!channel || channel->direction != TransferDirection::PrimaryToSecondary)
    return {};
  const auto ready = channel->channel.receive(ticket, static_cast<ID3D12Resource *>(native_destination), {});
  return {ready.fence, ready.value};
}
