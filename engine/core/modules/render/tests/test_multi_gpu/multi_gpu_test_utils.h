// multi_gpu_test_utils.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
// Shared helpers for the multi-GPU tests: device pairs, a copy queue per device and GPU-side completion.
#pragma once

#include <EASTL/allocator_malloc.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <dxgi1_4.h>
#include <gtest/gtest.h>

#include "multi_gpu_device.h"
#include "nau/directx/d3d12sdklayers.h"

namespace multi_gpu_test
{
    using Microsoft::WRL::ComPtr;

    // Standalone tests do not link the engine's default EASTL allocator.
    template <typename TValue>
    using TestVector = eastl::vector<TValue, eastl::allocator_malloc>;
    using TestString = eastl::basic_string<char, eastl::allocator_malloc>;

    struct TestGpu
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> gate;  // signaled from the CPU to hold back queued work
        ComPtr<ID3D12Fence> done;
    };

    inline bool makeGpu(IDXGIAdapter1* adapter, TestGpu& gpu)
    {
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device))))
        {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        return SUCCEEDED(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&gpu.queue))) &&
               SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.gate))) &&
               SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.done)));
    }

    // Two devices on two different physical adapters, in DXGI enumeration order.
    inline bool makeHardwarePair(TestGpu (&gpus)[2])
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            return false;
        }
        LUID first = {};
        int found = 0;
        for (UINT index = 0; found < 2; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            {
                return false;
            }
            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(adapter->GetDesc1(&desc)) || !drv3d_dx12::is_secondary_gpu_candidate(desc, first))
            {
                continue;
            }
            if (makeGpu(adapter.Get(), gpus[found]))
            {
                if (found == 0)
                {
                    first = desc.AdapterLuid;
                }
                ++found;
            }
        }
        return true;
    }

    // Any two distinct devices, enough for the bookkeeping that does not need two physical GPUs.
    // D3D12 devices are per-adapter singletons, so two devices need two adapters: a hardware pair if present,
    // otherwise WARP next to one hardware adapter.
    inline bool makeTwoDevices(TestGpu (&gpus)[2])
    {
        if (makeHardwarePair(gpus))
        {
            return true;
        }
        gpus[0] = {};
        gpus[1] = {};
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> warp;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            !makeGpu(warp.Get(), gpus[0]))
        {
            return false;
        }
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            {
                return false;
            }
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && drv3d_dx12::is_secondary_gpu_candidate(desc, {}) &&
                makeGpu(adapter.Get(), gpus[1]))
            {
                return true;
            }
        }
    }

    inline bool waitFence(ID3D12Fence* fence, uint64_t value, DWORD timeout_ms)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool reached = SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0;
        CloseHandle(event);
        return reached;
    }

    inline bool waitDone(TestGpu& gpu, uint64_t value, DWORD timeout_ms)
    {
        return waitFence(gpu.done.Get(), value, timeout_ms);
    }

    // The Nau pool intentionally uses WRITE_COMBINE upload heaps; only its performance warning may be allowed.
    // Resource/state/synchronization warnings and every ERROR/CORRUPTION message still fail the test.
    inline void expectNoDebugWarnings(ID3D12Device* device, bool allowWriteCombineWarning = false)
    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            return;
        }
        for (UINT64 index = 0; index < infoQueue->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            ASSERT_HRESULT_SUCCEEDED(infoQueue->GetMessage(index, nullptr, &size));
            TestVector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ASSERT_HRESULT_SUCCEEDED(infoQueue->GetMessage(index, message, &size));
            if (allowWriteCombineWarning && message->Severity == D3D12_MESSAGE_SEVERITY_WARNING &&
                message->ID == D3D12_MESSAGE_ID_WRITE_COMBINE_PERFORMANCE_WARNING)
            {
                continue;
            }
            EXPECT_GT(message->Severity, D3D12_MESSAGE_SEVERITY_WARNING) << message->pDescription;
        }
    }
}  // namespace multi_gpu_test
