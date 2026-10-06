// Copyright 2024 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>
#include "nau/directx/d3d12.h"
#include <wrl/client.h>

namespace drv3d_dx12
{
/**
 * @brief Fence shared between two D3D12 devices on different adapters.
 *
 * One device signals, the other one waits. Both operations are enqueued on command queues, so the CPU never
 * blocks on them. Values are issued by this object only, which keeps them monotonic.
 *
 * A wait is accepted only for a value whose signal is already enqueued. A queue waiting for a value that is never
 * signaled would stall that GPU until a device timeout, so such waits are refused instead.
 */
class CrossAdapterFence
{
public:
  enum class Side : uint32_t
  {
    SIGNALER,
    WAITER,
    COUNT
  };

private:
  Microsoft::WRL::ComPtr<ID3D12Fence> fences[static_cast<uint32_t>(Side::COUNT)];
  Microsoft::WRL::ComPtr<ID3D12Device> devices[static_cast<uint32_t>(Side::COUNT)];
  uint64_t issued = 0;

  // Compares COM identities, so a device passed through a newer interface version still matches.
  static bool belongsTo(ID3D12CommandQueue *queue, ID3D12Device *device)
  {
    Microsoft::WRL::ComPtr<IUnknown> owner;
    Microsoft::WRL::ComPtr<IUnknown> expected;
    return queue && device && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owner))) &&
           SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) && owner.Get() == expected.Get();
  }

public:
  CrossAdapterFence() = default;
  CrossAdapterFence(const CrossAdapterFence &) = delete;
  CrossAdapterFence &operator=(const CrossAdapterFence &) = delete;

  /**
   * @brief Creates the fence on the signaling device and opens it on the waiting device.
   * @param signaler Device whose queues signal the fence.
   * @param waiter Device whose queues wait for the fence.
   * @return S_OK, or the failing HRESULT; on failure the object stays empty.
   */
  HRESULT init(ID3D12Device *signaler, ID3D12Device *waiter)
  {
    reset();
    if (!signaler || !waiter || signaler == waiter)
      return E_INVALIDARG;

    Microsoft::WRL::ComPtr<ID3D12Fence> signalerFence;
    HRESULT hr = signaler->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
      IID_PPV_ARGS(&signalerFence));
    if (FAILED(hr))
      return hr;

    HANDLE handle = nullptr;
    hr = signaler->CreateSharedHandle(signalerFence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr))
      return hr;
    Microsoft::WRL::ComPtr<ID3D12Fence> waiterFence;
    hr = waiter->OpenSharedHandle(handle, IID_PPV_ARGS(&waiterFence));
    CloseHandle(handle);
    if (FAILED(hr))
      return hr;

    fences[static_cast<uint32_t>(Side::SIGNALER)] = signalerFence;
    fences[static_cast<uint32_t>(Side::WAITER)] = waiterFence;
    devices[static_cast<uint32_t>(Side::SIGNALER)] = signaler;
    devices[static_cast<uint32_t>(Side::WAITER)] = waiter;
    return S_OK;
  }

  void reset()
  {
    for (auto &fence : fences)
      fence.Reset();
    for (auto &device : devices)
      device.Reset();
    issued = 0;
  }

  explicit operator bool() const { return static_cast<bool>(fences[static_cast<uint32_t>(Side::SIGNALER)]); }

  /**
   * @brief Enqueues a signal of the next value on a queue of the signaling device.
   * @return The signaled value, or 0 if the fence is empty, the queue belongs to another device or Signal failed.
   */
  uint64_t enqueueSignal(ID3D12CommandQueue *queue)
  {
    if (!*this || !belongsTo(queue, devices[static_cast<uint32_t>(Side::SIGNALER)].Get()))
      return 0;
    if (FAILED(queue->Signal(fences[static_cast<uint32_t>(Side::SIGNALER)].Get(), issued + 1)))
      return 0;
    return ++issued;
  }

  /**
   * @brief Makes a queue of the waiting device wait on the GPU until value is signaled.
   * @return false if the fence is empty, the queue belongs to another device, value is 0 or not enqueued yet,
   *         or Wait failed. Nothing is enqueued in that case.
   */
  bool enqueueWait(ID3D12CommandQueue *queue, uint64_t value)
  {
    if (!*this || value == 0 || value > issued || !belongsTo(queue, devices[static_cast<uint32_t>(Side::WAITER)].Get()))
      return false;
    return SUCCEEDED(queue->Wait(fences[static_cast<uint32_t>(Side::WAITER)].Get(), value));
  }

  /**
   * @brief Last value the signaling GPU has reached. UINT64_MAX means the signaling device was removed.
   */
  uint64_t completedValue() const
  {
    return *this ? fences[static_cast<uint32_t>(Side::SIGNALER)]->GetCompletedValue() : 0;
  }

  uint64_t issuedValue() const { return issued; }

  /**
   * @brief True once the signaling device is removed. The runtime then completes the fence with UINT64_MAX on both
   *        sides, which releases waiting queues: they do not hang, but whatever they read after the wait is invalid.
   */
  bool isSignalerLost() const { return *this && completedValue() == UINT64_MAX; }

  bool isCompleted(uint64_t value) const { return value <= completedValue(); }

  /**
   * @brief Blocks the CPU until value is reached. Meant for teardown only.
   * @return S_OK, WAIT_TIMEOUT as HRESULT on timeout, E_INVALIDARG for a value that is not enqueued.
   */
  HRESULT waitOnCpu(uint64_t value, DWORD timeout_ms) const
  {
    if (!*this || value > issued)
      return E_INVALIDARG;
    if (isCompleted(value))
      return S_OK;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = fences[static_cast<uint32_t>(Side::SIGNALER)]->SetEventOnCompletion(value, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0)
      hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    CloseHandle(event);
    return hr;
  }

  /**
   * @brief Native fence object as seen by one side, for handing a sync point to that device's own queues.
   */
  ID3D12Fence *getFence(Side side) const { return fences[static_cast<uint32_t>(side)].Get(); }
};
} // namespace drv3d_dx12
