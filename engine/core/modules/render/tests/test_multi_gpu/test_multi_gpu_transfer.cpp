#include "multi_gpu_test_utils.h"
#include "multi_gpu_transfer.h"

#include <cstring>
#include <dxgidebug.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace drv3d_dx12;
using namespace multi_gpu_test;

namespace
{
D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  return desc;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

ComPtr<ID3D12Resource> create_resource(TestGpu &gpu, const D3D12_RESOURCE_DESC &desc,
  D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT)
{
  D3D12_HEAP_PROPERTIES properties = {};
  properties.Type = heap;
  const D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                      : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                         : D3D12_RESOURCE_STATE_COMMON;
  ComPtr<ID3D12Resource> resource;
  EXPECT_HRESULT_SUCCEEDED(
    gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
  return resource;
}

// Records one command list on the test copy queue, executes it and waits on the CPU.
template <typename Record>
void run_copy(TestGpu &gpu, Record record)
{
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ASSERT_HRESULT_SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)));
  ASSERT_HRESULT_SUCCEEDED(
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  record(list.Get());
  ASSERT_HRESULT_SUCCEEDED(list->Close());
  ID3D12CommandList *lists[] = {list.Get()};
  gpu.queue->ExecuteCommandLists(1, lists);
  const uint64_t value = gpu.done->GetCompletedValue() + 1;
  ASSERT_HRESULT_SUCCEEDED(gpu.queue->Signal(gpu.done.Get(), value));
  ASSERT_TRUE(wait_done(gpu, value, 10000));
}

std::vector<uint8_t> make_pattern(size_t size, uint32_t seed)
{
  std::vector<uint8_t> bytes(size);
  uint32_t state = seed * 2654435761u + 1;
  for (uint8_t &byte : bytes)
  {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<uint8_t>(state);
  }
  return bytes;
}

// Tightly packed bytes of subresource 0 <-> resource, through an upload or readback buffer.
struct Packing
{
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  UINT rows = 1;
  UINT64 rowBytes = 0;
  UINT64 total = 0;

  Packing(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc)
  {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
      rowBytes = total = desc.Width;
      return;
    }
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &total);
  }
  size_t packedSize() const { return static_cast<size_t>(rowBytes * rows); }
  UINT64 pitch() const { return layout.Footprint.RowPitch ? layout.Footprint.RowPitch : rowBytes; }
};

void copy_between(ID3D12GraphicsCommandList *list, ID3D12Resource *texture_or_buffer, ID3D12Resource *staging,
  const Packing &packing, bool to_staging)
{
  const D3D12_RESOURCE_DESC desc = texture_or_buffer->GetDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
  {
    if (to_staging)
      list->CopyBufferRegion(staging, 0, texture_or_buffer, 0, packing.total);
    else
      list->CopyBufferRegion(texture_or_buffer, 0, staging, 0, packing.total);
    return;
  }
  D3D12_TEXTURE_COPY_LOCATION resource = {};
  resource.pResource = texture_or_buffer;
  resource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION placed = {};
  placed.pResource = staging;
  placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  placed.PlacedFootprint = packing.layout;
  if (to_staging)
    list->CopyTextureRegion(&placed, 0, 0, 0, &resource, nullptr);
  else
    list->CopyTextureRegion(&resource, 0, 0, 0, &placed, nullptr);
}

void upload(TestGpu &gpu, ID3D12Resource *resource, const std::vector<uint8_t> &packed)
{
  const Packing packing(gpu.device.Get(), resource->GetDesc());
  ASSERT_EQ(packed.size(), packing.packedSize());
  auto staging = create_resource(gpu, buffer_desc(packing.total), D3D12_HEAP_TYPE_UPLOAD);
  uint8_t *data = nullptr;
  ASSERT_HRESULT_SUCCEEDED(staging->Map(0, nullptr, reinterpret_cast<void **>(&data)));
  for (UINT row = 0; row < packing.rows; ++row)
    memcpy(data + packing.layout.Offset + row * packing.pitch(), packed.data() + row * packing.rowBytes, packing.rowBytes);
  staging->Unmap(0, nullptr);
  run_copy(gpu, [&](ID3D12GraphicsCommandList *list) { copy_between(list, resource, staging.Get(), packing, false); });
}

std::vector<uint8_t> download(TestGpu &gpu, ID3D12Resource *resource)
{
  const Packing packing(gpu.device.Get(), resource->GetDesc());
  auto staging = create_resource(gpu, buffer_desc(packing.total), D3D12_HEAP_TYPE_READBACK);
  run_copy(gpu, [&](ID3D12GraphicsCommandList *list) { copy_between(list, resource, staging.Get(), packing, true); });
  std::vector<uint8_t> packed(packing.packedSize());
  uint8_t *data = nullptr;
  if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void **>(&data))))
    return {};
  for (UINT row = 0; row < packing.rows; ++row)
    memcpy(packed.data() + row * packing.rowBytes, data + packing.layout.Offset + row * packing.pitch(), packing.rowBytes);
  staging->Unmap(0, nullptr);
  return packed;
}

size_t count_mismatches(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
  if (a.size() != b.size())
    return SIZE_MAX;
  size_t count = 0;
  for (size_t i = 0; i < a.size(); ++i)
    count += a[i] != b[i];
  return count;
}

// Sends one pattern through a fresh channel and checks it arrives byte for byte.
void check_round(TestGpu &producer, TestGpu &consumer, TestGpu &heap_owner, const D3D12_RESOURCE_DESC &desc, uint32_t seed)
{
  CrossAdapterChannel channel;
  const HRESULT hr = channel.init(producer.device.Get(), consumer.device.Get(), heap_owner.device.Get(), desc, 2, L"test");
  ASSERT_HRESULT_SUCCEEDED(hr);
  auto source = create_resource(producer, desc);
  auto destination = create_resource(consumer, desc);
  const auto pattern = make_pattern(Packing(producer.device.Get(), desc).packedSize(), seed);
  upload(producer, source.Get(), pattern);

  const auto sent = channel.send(source.Get(), {});
  ASSERT_EQ(sent.ticket, 1u);
  ASSERT_NE(sent.sourceReleased.fence, nullptr);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {});
  ASSERT_NE(ready.fence, nullptr);
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));
  ASSERT_TRUE(wait_fence(sent.sourceReleased.fence, sent.sourceReleased.value, 10000));

  EXPECT_EQ(count_mismatches(download(consumer, destination.Get()), pattern), 0u);
}
} // namespace

TEST(MultiGpuTransfer, StartsEmptyAndRejectsInvalidSetup)
{
  CrossAdapterChannel channel;
  EXPECT_FALSE(channel);
  EXPECT_EQ(channel.send(nullptr, {}).ticket, 0u);
  EXPECT_EQ(channel.receive(1, nullptr, {}).fence, nullptr);
  channel.reset();

  TestGpu gpus[2];
  if (!make_two_devices(gpus))
    GTEST_SKIP() << "two distinct D3D12 devices unavailable";
  ID3D12Device *a = gpus[0].device.Get();
  ID3D12Device *b = gpus[1].device.Get();
  const auto color = texture_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);

  EXPECT_EQ(channel.init(nullptr, b, a, color, 2, nullptr), E_INVALIDARG);
  EXPECT_EQ(channel.init(a, nullptr, a, color, 2, nullptr), E_INVALIDARG);
  EXPECT_EQ(channel.init(a, a, a, color, 2, nullptr), E_INVALIDARG);
  EXPECT_EQ(channel.init(a, b, nullptr, color, 2, nullptr), E_INVALIDARG);
  EXPECT_EQ(channel.init(a, b, a, color, 1, nullptr), E_INVALIDARG); // needs two slots in flight
  EXPECT_EQ(channel.init(a, b, a, color, CrossAdapterChannel::MAX_SLOTS + 1, nullptr), E_INVALIDARG);

  auto msaa = color;
  msaa.SampleDesc.Count = 4;
  EXPECT_EQ(channel.init(a, b, a, msaa, 2, nullptr), E_INVALIDARG);
  auto volume = color;
  volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  EXPECT_EQ(channel.init(a, b, a, volume, 2, nullptr), E_INVALIDARG);
  auto empty = buffer_desc(0);
  EXPECT_EQ(channel.init(a, b, a, empty, 2, nullptr), E_INVALIDARG);
  // Two planes: depth and stencil cannot be moved as one image.
  EXPECT_EQ(channel.init(a, b, a, texture_desc(64, 64, DXGI_FORMAT_D24_UNORM_S8_UINT), 2, nullptr), E_INVALIDARG);
  EXPECT_FALSE(channel);
}

TEST(MultiGpuTransferHardware, MovesTexturesBothWaysByteForByte)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = texture_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
  check_round(gpus[0], gpus[1], gpus[0], desc, 1);
  check_round(gpus[1], gpus[0], gpus[0], desc, 2);
  check_round(gpus[0], gpus[1], gpus[1], desc, 3);
}

TEST(MultiGpuTransferHardware, MovesFormatsTheTeamsAskedFor)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  // DOF: RGBA16F colour, R32F depth, R16F weights; AO: R8 result. Odd sizes catch row pitch mistakes.
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT,
    DXGI_FORMAT_R8_UNORM};
  uint32_t seed = 10;
  for (DXGI_FORMAT format : formats)
  {
    SCOPED_TRACE(testing::Message() << "DXGI format " << format);
    check_round(gpus[0], gpus[1], gpus[0], texture_desc(961, 541, format), seed++);
    check_round(gpus[1], gpus[0], gpus[0], texture_desc(961, 541, format), seed++);
  }
}

TEST(MultiGpuTransferHardware, MovesBuffersOfAnySize)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  check_round(gpus[0], gpus[1], gpus[0], buffer_desc(64), 20);
  check_round(gpus[1], gpus[0], gpus[0], buffer_desc(3 * 1024 * 1024 + 4), 21);
}

TEST(MultiGpuTransferHardware, MovesBuffersAtAnOffsetInsideLargerResources)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  // Engine buffers are sub-allocated: the payload sits at an offset inside a bigger resource.
  constexpr UINT64 payloadSize = 4096 + 12;
  constexpr UINT64 sourceOffset = 65536 + 256;
  constexpr UINT64 destinationOffset = 768;
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(
    channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), buffer_desc(payloadSize), 2, nullptr));
  auto source = create_resource(gpus[0], buffer_desc(256 * 1024));
  auto destination = create_resource(gpus[1], buffer_desc(64 * 1024));
  const auto sourceBytes = make_pattern(256 * 1024, 70);
  const auto destinationBytes = make_pattern(64 * 1024, 71);
  upload(gpus[0], source.Get(), sourceBytes);
  upload(gpus[1], destination.Get(), destinationBytes);

  EXPECT_EQ(channel.send(source.Get(), {}, 256 * 1024 - payloadSize + 1).ticket, 0u); // past the end
  EXPECT_EQ(channel.send(source.Get(), {}, UINT64_MAX).ticket, 0u);
  const auto sent = channel.send(source.Get(), {}, sourceOffset);
  ASSERT_EQ(sent.ticket, 1u);
  EXPECT_EQ(channel.receive(sent.ticket, destination.Get(), {}, 64 * 1024 - payloadSize + 1).fence, nullptr);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {}, destinationOffset);
  ASSERT_NE(ready.fence, nullptr);
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));

  // Only the payload range changed; the bytes around it are untouched.
  auto expected = destinationBytes;
  memcpy(expected.data() + destinationOffset, sourceBytes.data() + sourceOffset, payloadSize);
  EXPECT_EQ(count_mismatches(download(gpus[1], destination.Get()), expected), 0u);
}

TEST(MultiGpuTransferHardware, RejectsOffsetsForTextures)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = texture_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));
  auto source = create_resource(gpus[0], desc);
  EXPECT_EQ(channel.send(source.Get(), {}, 256).ticket, 0u);
}

TEST(MultiGpuTransferHardware, AcceptsEndpointsWithTheSameByteLayoutInAnotherFormat)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  // The engine creates textures in typeless formats, a technique on the other GPU usually in typed ones.
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(),
    texture_desc(333, 77, DXGI_FORMAT_R8G8B8A8_UNORM), 2, nullptr));
  auto source = create_resource(gpus[0], texture_desc(333, 77, DXGI_FORMAT_R8G8B8A8_TYPELESS));
  auto destination = create_resource(gpus[1], texture_desc(333, 77, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
  const auto pattern = make_pattern(333 * 77 * 4, 80);
  upload(gpus[0], source.Get(), pattern);
  const auto sent = channel.send(source.Get(), {});
  ASSERT_EQ(sent.ticket, 1u);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {});
  ASSERT_NE(ready.fence, nullptr);
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));
  EXPECT_EQ(count_mismatches(download(gpus[1], destination.Get()), pattern), 0u);
}

TEST(MultiGpuTransferHardware, MovesDepthIntoAColorTexture)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  // DOF and AO move a single-plane depth buffer and read it as R32F on the other GPU.
  auto depthDesc = texture_desc(320, 180, DXGI_FORMAT_D32_FLOAT);
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), depthDesc, 2, nullptr));
  auto source = create_resource(gpus[0], depthDesc);
  auto destination = create_resource(gpus[1], texture_desc(320, 180, DXGI_FORMAT_R32_FLOAT));
  std::vector<uint8_t> pattern(320 * 180 * 4);
  for (size_t i = 0; i < 320 * 180; ++i)
  {
    const float depth = static_cast<float>(i % 1000) / 1000.0f;
    memcpy(pattern.data() + i * 4, &depth, 4);
  }
  upload(gpus[0], source.Get(), pattern);
  const auto sent = channel.send(source.Get(), {});
  ASSERT_EQ(sent.ticket, 1u);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {});
  ASSERT_NE(ready.fence, nullptr);
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));
  EXPECT_EQ(count_mismatches(download(gpus[1], destination.Get()), pattern), 0u);
}

TEST(MultiGpuTransferHardware, RejectsEndpointsThatDoNotMatch)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = texture_desc(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM);
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));
  auto onConsumer = create_resource(gpus[1], desc);
  auto wrongSize = create_resource(gpus[0], texture_desc(64, 128, DXGI_FORMAT_R8G8B8A8_UNORM));
  auto wrongFormat = create_resource(gpus[0], texture_desc(128, 128, DXGI_FORMAT_R16G16B16A16_FLOAT));
  EXPECT_EQ(channel.send(nullptr, {}).ticket, 0u);
  EXPECT_EQ(channel.send(onConsumer.Get(), {}).ticket, 0u); // source must live on the producer
  EXPECT_EQ(channel.send(wrongSize.Get(), {}).ticket, 0u);
  EXPECT_EQ(channel.send(wrongFormat.Get(), {}).ticket, 0u);

  auto source = create_resource(gpus[0], desc);
  const auto sent = channel.send(source.Get(), {});
  ASSERT_EQ(sent.ticket, 1u);
  EXPECT_EQ(channel.receive(sent.ticket, source.Get(), {}).fence, nullptr); // destination must live on the consumer
  EXPECT_EQ(channel.receive(0, onConsumer.Get(), {}).fence, nullptr);
  EXPECT_EQ(channel.receive(2, onConsumer.Get(), {}).fence, nullptr); // not sent yet
  const auto ready = channel.receive(1, onConsumer.Get(), {});
  ASSERT_NE(ready.fence, nullptr);
  EXPECT_TRUE(wait_fence(ready.fence, ready.value, 10000));
}

TEST(MultiGpuTransferHardware, LateConsumerDropsFramesInsteadOfHangingProducer)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = texture_desc(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM);
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));
  ComPtr<ID3D12Resource> sources[3];
  for (uint32_t i = 0; i < 3; ++i)
  {
    sources[i] = create_resource(gpus[0], desc);
    upload(gpus[0], sources[i].Get(), make_pattern(256 * 256 * 4, 30 + i));
  }
  auto destination = create_resource(gpus[1], desc);

  EXPECT_EQ(channel.send(sources[0].Get(), {}).ticket, 1u);
  EXPECT_EQ(channel.send(sources[1].Get(), {}).ticket, 2u);
  // Both slots hold frames nobody has asked for: the third frame is dropped, nothing waits.
  EXPECT_EQ(channel.send(sources[2].Get(), {}).ticket, 0u);
  EXPECT_EQ(channel.send(sources[2].Get(), {}).ticket, 0u);

  // The consumer jumps to the newest frame. Frame 1 is skipped for good, so its slot is free without a wait.
  const auto second = channel.receive(2, destination.Get(), {});
  ASSERT_NE(second.fence, nullptr);
  const auto third = channel.send(sources[2].Get(), {});
  EXPECT_EQ(third.ticket, 3u);
  EXPECT_EQ(channel.receive(1, destination.Get(), {}).fence, nullptr); // overwritten by frame 3

  ASSERT_TRUE(wait_fence(second.fence, second.value, 10000));
  EXPECT_EQ(count_mismatches(download(gpus[1], destination.Get()), make_pattern(256 * 256 * 4, 31)), 0u);
  const auto ready = channel.receive(3, destination.Get(), {});
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));
  EXPECT_EQ(count_mismatches(download(gpus[1], destination.Get()), make_pattern(256 * 256 * 4, 32)), 0u);
}

TEST(MultiGpuTransferHardware, StreamsManyFramesWithoutBlockingCpu)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = texture_desc(320, 240, DXGI_FORMAT_R8G8B8A8_UNORM);
  const size_t size = 320 * 240 * 4;
  constexpr uint32_t ring = 4;
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));

  ComPtr<ID3D12Resource> sources[ring], destinations[ring];
  for (uint32_t i = 0; i < ring; ++i)
  {
    sources[i] = create_resource(gpus[0], desc);
    destinations[i] = create_resource(gpus[1], desc);
    upload(gpus[0], sources[i].Get(), make_pattern(size, 40 + i));
  }

  // First frames: the producer GPU holds them behind a gate that the CPU opens only after all calls returned.
  // A channel that waited on the CPU anywhere would deadlock here.
  uint32_t frame = 0;
  CrossAdapterSyncPoint lastReady;
  for (; frame < 6; ++frame)
  {
    const uint32_t i = frame % ring;
    const auto sent = channel.send(sources[i].Get(), {gpus[0].gate.Get(), 1});
    ASSERT_EQ(sent.ticket, frame + 1);
    lastReady = channel.receive(sent.ticket, destinations[i].Get(), {});
    ASSERT_NE(lastReady.fence, nullptr);
  }
  Sleep(50);
  EXPECT_LT(lastReady.fence->GetCompletedValue(), lastReady.value);
  ASSERT_HRESULT_SUCCEEDED(gpus[0].gate->Signal(1));

  // Then a steady stream paced like an engine with two frames in flight.
  CrossAdapterSyncPoint inFlight[2];
  for (; frame < 200; ++frame)
  {
    CrossAdapterSyncPoint &oldest = inFlight[frame % 2];
    if (oldest.fence)
      ASSERT_TRUE(wait_fence(oldest.fence, oldest.value, 10000));
    const uint32_t i = frame % ring;
    const auto sent = channel.send(sources[i].Get(), {});
    ASSERT_EQ(sent.ticket, frame + 1);
    oldest = lastReady = channel.receive(sent.ticket, destinations[i].Get(), {});
    ASSERT_NE(lastReady.fence, nullptr);
  }
  ASSERT_TRUE(wait_fence(lastReady.fence, lastReady.value, 10000));
  for (uint32_t i = 0; i < ring; ++i)
    EXPECT_EQ(count_mismatches(download(gpus[1], destinations[i].Get()), make_pattern(size, 40 + i)), 0u) << i;
}

TEST(MultiGpuTransferHardware, RefusesInsteadOfBlockingWhenCpuRunsTooFarAhead)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = buffer_desc(1024);
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[0].device.Get(), gpus[1].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));
  auto source = create_resource(gpus[0], desc);
  auto destination = create_resource(gpus[1], desc);

  uint32_t accepted = 0;
  CrossAdapterSyncPoint lastReady;
  for (uint32_t frame = 0; frame < 3 * CrossAdapterChannel::MAX_RECORDINGS; ++frame)
  {
    const auto sent = channel.send(source.Get(), {gpus[0].gate.Get(), 1});
    if (sent.ticket == 0)
      break;
    ++accepted;
    lastReady = channel.receive(sent.ticket, destination.Get(), {});
    ASSERT_NE(lastReady.fence, nullptr);
  }
  EXPECT_EQ(accepted, CrossAdapterChannel::MAX_RECORDINGS);

  // Once the GPUs catch up, the channel accepts work again.
  ASSERT_HRESULT_SUCCEEDED(gpus[0].gate->Signal(1));
  ASSERT_TRUE(wait_fence(lastReady.fence, lastReady.value, 10000));
  const auto sent = channel.send(source.Get(), {});
  EXPECT_EQ(sent.ticket, accepted + 1);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {});
  ASSERT_NE(ready.fence, nullptr);
  // The channel does not own the endpoints: keep them alive until the GPUs are done with them.
  EXPECT_TRUE(wait_fence(ready.fence, ready.value, 10000));
}

TEST(MultiGpuTransferHardware, WaitsForTheTechniqueBeforeTouchingItsResources)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  const auto desc = buffer_desc(4096);
  CrossAdapterChannel channel;
  ASSERT_HRESULT_SUCCEEDED(channel.init(gpus[1].device.Get(), gpus[0].device.Get(), gpus[0].device.Get(), desc, 2, nullptr));
  auto source = create_resource(gpus[1], desc);
  auto destination = create_resource(gpus[0], desc);
  upload(gpus[1], source.Get(), make_pattern(4096, 50));

  // Sync points from the wrong device are refused instead of being enqueued.
  EXPECT_EQ(channel.send(source.Get(), {gpus[0].gate.Get(), 1}).ticket, 0u);

  const auto sent = channel.send(source.Get(), {gpus[1].gate.Get(), 1});
  ASSERT_EQ(sent.ticket, 1u);
  const auto ready = channel.receive(sent.ticket, destination.Get(), {gpus[0].gate.Get(), 1});
  ASSERT_NE(ready.fence, nullptr);
  Sleep(50);
  EXPECT_LT(sent.sourceReleased.fence->GetCompletedValue(), sent.sourceReleased.value);

  ASSERT_HRESULT_SUCCEEDED(gpus[1].gate->Signal(1));
  ASSERT_TRUE(wait_fence(sent.sourceReleased.fence, sent.sourceReleased.value, 10000));
  Sleep(50);
  EXPECT_LT(ready.fence->GetCompletedValue(), ready.value); // still held by the consumer's own gate

  ASSERT_HRESULT_SUCCEEDED(gpus[0].gate->Signal(1));
  ASSERT_TRUE(wait_fence(ready.fence, ready.value, 10000));
  EXPECT_EQ(count_mismatches(download(gpus[0], destination.Get()), make_pattern(4096, 50)), 0u);
}

TEST(MultiGpuTransferHardware, DebugLayerReportsNothingAndNothingLeaks)
{
  ComPtr<ID3D12Debug> debug;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    GTEST_SKIP() << "D3D12 debug layer unavailable";
  debug->EnableDebugLayer();
  ComPtr<IDXGIInfoQueue> dxgiQueue;
  ComPtr<IDXGIDebug1> dxgiDebug;
  if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiQueue))) || FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    GTEST_SKIP() << "DXGI debug diagnostics unavailable";
  dxgiQueue->ClearStoredMessages(DXGI_DEBUG_ALL);

  {
    TestGpu gpus[2];
    if (!make_hardware_pair(gpus))
      GTEST_SKIP() << "Two physical D3D12 adapters are required";
    check_round(gpus[0], gpus[1], gpus[0], texture_desc(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT), 60);
    check_round(gpus[1], gpus[0], gpus[0], texture_desc(640, 360, DXGI_FORMAT_R8_UNORM), 61);
    check_round(gpus[1], gpus[0], gpus[1], buffer_desc(1000), 62);
    for (TestGpu &gpu : gpus)
      expect_no_debug_warnings(gpu.device.Get());
  }

  ASSERT_HRESULT_SUCCEEDED(dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL,
    static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL)));
  for (UINT64 index = 0; index < dxgiQueue->GetNumStoredMessages(DXGI_DEBUG_ALL); ++index)
  {
    SIZE_T size = 0;
    ASSERT_HRESULT_SUCCEEDED(dxgiQueue->GetMessage(DXGI_DEBUG_ALL, index, nullptr, &size));
    std::vector<char> storage(size);
    auto *message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE *>(storage.data());
    ASSERT_HRESULT_SUCCEEDED(dxgiQueue->GetMessage(DXGI_DEBUG_ALL, index, message, &size));
    const std::string description(message->pDescription, message->DescriptionByteLength);
    EXPECT_EQ(description.find("Live ID3D12"), std::string::npos) << description;
  }
}
