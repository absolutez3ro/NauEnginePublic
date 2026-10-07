#include "multi_gpu_fence.h"
#include "multi_gpu_test_utils.h"
#include "nau/directx/d3d12sdklayers.h"

#include <dxgidebug.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace drv3d_dx12;
using namespace multi_gpu_test;

namespace
{
// Signaler holds its signal behind its gate; waiter waits for the signal on the GPU, then marks done = 1.
void check_gpu_side_wait(TestGpu &signaler, TestGpu &waiter)
{
  CrossAdapterFence fence;
  ASSERT_HRESULT_SUCCEEDED(fence.init(signaler.device.Get(), waiter.device.Get()));

  ASSERT_HRESULT_SUCCEEDED(signaler.queue->Wait(signaler.gate.Get(), 1));
  const uint64_t value = fence.enqueueSignal(signaler.queue.Get());
  ASSERT_EQ(value, 1u);
  ASSERT_TRUE(fence.enqueueWait(waiter.queue.Get(), value));
  ASSERT_HRESULT_SUCCEEDED(waiter.queue->Signal(waiter.done.Get(), 1));

  // The CPU returned from every call above while the signal is still held back.
  Sleep(100);
  EXPECT_FALSE(fence.isCompleted(value));
  EXPECT_EQ(waiter.done->GetCompletedValue(), 0u);
  EXPECT_EQ(fence.waitOnCpu(value, 0), HRESULT_FROM_WIN32(WAIT_TIMEOUT));

  ASSERT_HRESULT_SUCCEEDED(signaler.gate->Signal(1));
  EXPECT_TRUE(wait_done(waiter, 1, 5000));
  EXPECT_TRUE(fence.isCompleted(value));
  EXPECT_FALSE(fence.isSignalerLost());
}
} // namespace

TEST(MultiGpuFence, StartsEmptyAndRejectsInvalidDevices)
{
  CrossAdapterFence fence;
  EXPECT_FALSE(fence);
  EXPECT_EQ(fence.enqueueSignal(nullptr), 0u);
  EXPECT_FALSE(fence.enqueueWait(nullptr, 1));
  EXPECT_EQ(fence.completedValue(), 0u);
  EXPECT_FALSE(fence.isSignalerLost());
  EXPECT_EQ(fence.waitOnCpu(0, 0), E_INVALIDARG);

  TestGpu gpus[2];
  if (!make_two_devices(gpus))
    GTEST_SKIP() << "two distinct D3D12 devices unavailable";
  EXPECT_EQ(fence.init(nullptr, gpus[1].device.Get()), E_INVALIDARG);
  EXPECT_EQ(fence.init(gpus[0].device.Get(), nullptr), E_INVALIDARG);
  EXPECT_EQ(fence.init(gpus[0].device.Get(), gpus[0].device.Get()), E_INVALIDARG);
  EXPECT_FALSE(fence);
  fence.reset();
  fence.reset();
  EXPECT_FALSE(fence);
}

TEST(MultiGpuFence, RefusesForeignQueuesAndWaitsForValuesNotEnqueued)
{
  TestGpu gpus[2];
  if (!make_two_devices(gpus))
    GTEST_SKIP() << "two distinct D3D12 devices unavailable";
  CrossAdapterFence fence;
  if (FAILED(fence.init(gpus[0].device.Get(), gpus[1].device.Get())))
    GTEST_SKIP() << "shared cross-adapter fences unavailable";

  // Nothing enqueued yet: a wait would never be satisfied, so it is refused.
  EXPECT_FALSE(fence.enqueueWait(gpus[1].queue.Get(), 1));
  EXPECT_FALSE(fence.enqueueWait(gpus[1].queue.Get(), 0));
  EXPECT_EQ(fence.waitOnCpu(1, 0), E_INVALIDARG);
  // Each side accepts only its own device's queues.
  EXPECT_EQ(fence.enqueueSignal(gpus[1].queue.Get()), 0u);
  EXPECT_EQ(fence.issuedValue(), 0u);

  EXPECT_EQ(fence.enqueueSignal(gpus[0].queue.Get()), 1u);
  EXPECT_EQ(fence.enqueueSignal(gpus[0].queue.Get()), 2u);
  EXPECT_FALSE(fence.enqueueWait(gpus[0].queue.Get(), 2));
  EXPECT_FALSE(fence.enqueueWait(gpus[1].queue.Get(), 3));
  EXPECT_TRUE(fence.enqueueWait(gpus[1].queue.Get(), 2));
  EXPECT_HRESULT_SUCCEEDED(fence.waitOnCpu(2, 5000));
  EXPECT_TRUE(fence.isCompleted(2));

  // A queue of the same device reached through a newer interface still counts as that device.
  ComPtr<ID3D12Device1> device1;
  if (SUCCEEDED(gpus[0].device.As(&device1)))
  {
    CrossAdapterFence viaNewer;
    ASSERT_HRESULT_SUCCEEDED(viaNewer.init(device1.Get(), gpus[1].device.Get()));
    EXPECT_EQ(viaNewer.enqueueSignal(gpus[0].queue.Get()), 1u);
  }

  fence.reset();
  EXPECT_FALSE(fence);
  EXPECT_EQ(fence.issuedValue(), 0u);
}

TEST(MultiGpuFenceHardware, GpuWaitsForOtherAdapterWithoutBlockingCpu)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  check_gpu_side_wait(gpus[0], gpus[1]);
}

TEST(MultiGpuFenceHardware, GpuWaitsInReverseDirection)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  check_gpu_side_wait(gpus[1], gpus[0]);
}

TEST(MultiGpuFenceHardware, ManySignalsStayOrdered)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  CrossAdapterFence fence;
  ASSERT_HRESULT_SUCCEEDED(fence.init(gpus[0].device.Get(), gpus[1].device.Get()));
  for (uint64_t i = 1; i <= 200; ++i)
  {
    ASSERT_EQ(fence.enqueueSignal(gpus[0].queue.Get()), i);
    ASSERT_TRUE(fence.enqueueWait(gpus[1].queue.Get(), i));
    ASSERT_HRESULT_SUCCEEDED(gpus[1].queue->Signal(gpus[1].done.Get(), i));
  }
  EXPECT_HRESULT_SUCCEEDED(fence.waitOnCpu(200, 5000));
  EXPECT_TRUE(wait_done(gpus[1], 200, 5000));
  EXPECT_EQ(fence.completedValue(), 200u);
}

TEST(MultiGpuFenceHardware, RemovedSignalerReleasesWaiter)
{
  TestGpu gpus[2];
  if (!make_hardware_pair(gpus))
    GTEST_SKIP() << "Two physical D3D12 adapters are required";
  ComPtr<ID3D12Device5> removable;
  if (FAILED(gpus[1].device.As(&removable)))
    GTEST_SKIP() << "ID3D12Device5::RemoveDevice unavailable";

  CrossAdapterFence fence;
  ASSERT_HRESULT_SUCCEEDED(fence.init(gpus[1].device.Get(), gpus[0].device.Get()));
  ASSERT_HRESULT_SUCCEEDED(gpus[1].queue->Wait(gpus[1].gate.Get(), 1)); // never released
  const uint64_t value = fence.enqueueSignal(gpus[1].queue.Get());
  ASSERT_TRUE(fence.enqueueWait(gpus[0].queue.Get(), value));
  ASSERT_HRESULT_SUCCEEDED(gpus[0].queue->Signal(gpus[0].done.Get(), 1));
  Sleep(100);
  ASSERT_EQ(gpus[0].done->GetCompletedValue(), 0u);

  removable->RemoveDevice();

  // The waiting GPU must not hang: the runtime completes the shared fence on both sides.
  EXPECT_TRUE(wait_done(gpus[0], 1, 5000));
  EXPECT_TRUE(fence.isSignalerLost());
  EXPECT_EQ(fence.getFence(CrossAdapterFence::Side::WAITER)->GetCompletedValue(), UINT64_MAX);
  EXPECT_HRESULT_SUCCEEDED(gpus[0].device->GetDeviceRemovedReason());
}

TEST(MultiGpuFenceHardware, DebugLayerReportsNothingAndNothingLeaks)
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
    check_gpu_side_wait(gpus[0], gpus[1]);
    check_gpu_side_wait(gpus[1], gpus[0]);
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
    EXPECT_EQ(description.find("Live ID3D12Fence"), std::string::npos) << description;
    EXPECT_EQ(description.find("Live ID3D12Device"), std::string::npos) << description;
  }
}
