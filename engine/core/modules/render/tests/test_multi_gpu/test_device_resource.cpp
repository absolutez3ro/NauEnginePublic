// Copyright 2026 N-GINN LLC. All rights reserved.

#include "device_resource.h"
#include "multi_gpu_device.h"
#include "nau/directx/d3d12sdklayers.h"

#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

using drv3d_dx12::DeviceResource;
using Microsoft::WRL::ComPtr;

namespace
{
void expect_owner(const DeviceResource& resource, ID3D12Device* device)
{
    ASSERT_NE(resource.getResource(), nullptr);
    EXPECT_EQ(resource.getDevice(), device);
    ComPtr<ID3D12Device> actual;
    ASSERT_HRESULT_SUCCEEDED(resource.getResource()->GetDevice(IID_PPV_ARGS(&actual)));
    EXPECT_EQ(actual.Get(), device);
}

ComPtr<ID3D12Device> create_test_device(bool warp)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        return {};
    }
    ComPtr<ID3D12Device> device;
    if (warp)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
        {
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        }
        return device;
    }
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(index, &adapter)))
        {
            return {};
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc)) || drv3d_dx12::is_software_device(desc))
        {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            return device;
        }
    }
}

class DeviceResourceTest : public ::testing::TestWithParam<bool>
{
protected:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> diagnostics;

    void SetUp() override
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
        }
        device = create_test_device(GetParam());
        if (!device)
        {
            GTEST_SKIP() << (GetParam() ? "D3D12 WARP unavailable" : "D3D12 hardware unavailable");
        }
        if (debug)
        {
            ASSERT_HRESULT_SUCCEEDED(device.As(&diagnostics));
            diagnostics->ClearStoredMessages();
        }
    }

    void TearDown() override
    {
        if (!diagnostics)
        {
            return;
        }
        // Test-local resources and command objects have already been destroyed here.
        ComPtr<ID3D12DebugDevice> debugDevice;
        ASSERT_HRESULT_SUCCEEDED(device.As(&debugDevice));
        EXPECT_HRESULT_SUCCEEDED(debugDevice->ReportLiveDeviceObjects(
            static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)));
        for (UINT64 index = 0; index < diagnostics->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            ASSERT_HRESULT_SUCCEEDED(diagnostics->GetMessage(index, nullptr, &size));
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ASSERT_HRESULT_SUCCEEDED(diagnostics->GetMessage(index, message, &size));
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
            EXPECT_NE(message->ID, D3D12_MESSAGE_ID_LIVE_RESOURCE) << message->pDescription;
            EXPECT_NE(message->ID, D3D12_MESSAGE_ID_LIVE_HEAP) << message->pDescription;
        }
    }
};

// Only the tests submit work. Production resources do not own a queue or hide a GPU wait.
class TestCommands
{
public:
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;

    void init(ID3D12Device* device)
    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ASSERT_HRESULT_SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        ASSERT_HRESULT_SUCCEEDED(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)));
        ASSERT_HRESULT_SUCCEEDED(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        ASSERT_HRESULT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }

    void transition(const DeviceResource& resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource.getResource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

    void submitAndWait()
    {
        ASSERT_HRESULT_SUCCEEDED(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ASSERT_HRESULT_SUCCEEDED(queue->Signal(fence.Get(), 1));
        // A null event blocks until completion; CTest's timeout bounds a hung driver.
        ASSERT_HRESULT_SUCCEEDED(fence->SetEventOnCompletion(1, nullptr));
        ASSERT_EQ(fence->GetCompletedValue(), 1u);
    }
};
}

TEST(DeviceResourceInput, UnavailableDeviceDoesNotFallBackToPrimary)
{
    DeviceResource resource;
    EXPECT_EQ(resource.createRenderTarget(nullptr, 16, 16), E_INVALIDARG);
    EXPECT_EQ(resource.createBuffer(nullptr, 256, D3D12_HEAP_TYPE_DEFAULT), E_INVALIDARG);
    EXPECT_EQ(resource.getResource(), nullptr);
    EXPECT_EQ(resource.getDevice(), nullptr);
    resource.reset();
    resource.reset();
}

TEST_P(DeviceResourceTest, RejectsInvalidDescriptionsWithoutAllocating)
{
    DeviceResource resource;
    EXPECT_EQ(resource.createRenderTarget(device.Get(), 0, 16), E_INVALIDARG);
    EXPECT_EQ(resource.createRenderTarget(device.Get(), 16, 0), E_INVALIDARG);
    EXPECT_EQ(resource.createRenderTarget(device.Get(), D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1, 16), E_INVALIDARG);
    EXPECT_EQ(resource.createRenderTarget(device.Get(), 16, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1), E_INVALIDARG);
    EXPECT_EQ(resource.createBuffer(device.Get(), 0, D3D12_HEAP_TYPE_DEFAULT), E_INVALIDARG);
    EXPECT_EQ(resource.createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_CUSTOM), E_INVALIDARG);
    EXPECT_EQ(resource.getResource(), nullptr);
    EXPECT_EQ(resource.getDevice(), nullptr);
}

TEST_P(DeviceResourceTest, PreservesOwnerUntilExplicitReset)
{
    DeviceResource resource;
    ASSERT_HRESULT_SUCCEEDED(resource.createRenderTarget(device.Get(), 37, 19));
    expect_owner(resource, device.Get());
    auto* original = resource.getResource();
    const auto desc = original->GetDesc();
    EXPECT_EQ(desc.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    EXPECT_EQ(desc.Width, 37u);
    EXPECT_EQ(desc.Height, 19u);
    EXPECT_EQ(desc.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(desc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    EXPECT_EQ(resource.getInitialState(), D3D12_RESOURCE_STATE_COMMON);

    EXPECT_EQ(resource.createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD), DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(resource.createRenderTarget(nullptr, 16, 16), E_INVALIDARG);
    EXPECT_EQ(resource.getResource(), original);
    expect_owner(resource, device.Get());
    resource.reset();
    EXPECT_EQ(resource.getResource(), nullptr);
    EXPECT_EQ(resource.getDevice(), nullptr);
    for (auto heapType : {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK})
    {
        ASSERT_HRESULT_SUCCEEDED(resource.createBuffer(device.Get(), 1024, heapType));
        expect_owner(resource, device.Get());
        D3D12_HEAP_PROPERTIES heap = {};
        D3D12_HEAP_FLAGS flags;
        ASSERT_HRESULT_SUCCEEDED(resource.getResource()->GetHeapProperties(&heap, &flags));
        EXPECT_EQ(heap.Type, heapType);
        EXPECT_EQ(heap.CreationNodeMask, 1u);
        EXPECT_EQ(heap.VisibleNodeMask, 1u);
        // The runtime adds resource-category restrictions to an implicit committed heap.
        EXPECT_EQ(flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER), 0);
        resource.reset();
    }
}

TEST_P(DeviceResourceTest, UploadDefaultReadbackRoundTrip)
{
    std::array<unsigned char, 1024> expected;
    for (size_t index = 0; index < expected.size(); ++index)
    {
        expected[index] = static_cast<unsigned char>(index * 17 + 3);
    }
    DeviceResource upload, gpu, readback;
    ASSERT_HRESULT_SUCCEEDED(upload.createBuffer(device.Get(), expected.size(), D3D12_HEAP_TYPE_UPLOAD));
    ASSERT_HRESULT_SUCCEEDED(gpu.createBuffer(device.Get(), expected.size(), D3D12_HEAP_TYPE_DEFAULT));
    ASSERT_HRESULT_SUCCEEDED(readback.createBuffer(device.Get(), expected.size(), D3D12_HEAP_TYPE_READBACK));
    EXPECT_EQ(upload.getInitialState(), D3D12_RESOURCE_STATE_GENERIC_READ);
    EXPECT_EQ(readback.getInitialState(), D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    ASSERT_HRESULT_SUCCEEDED(upload.getResource()->Map(0, &noRead, &mapped));
    std::memcpy(mapped, expected.data(), expected.size());
    upload.getResource()->Unmap(0, nullptr);

    TestCommands commands;
    ASSERT_NO_FATAL_FAILURE(commands.init(device.Get()));
    commands.transition(gpu, gpu.getInitialState(), D3D12_RESOURCE_STATE_COPY_DEST);
    commands.list->CopyBufferRegion(gpu.getResource(), 0, upload.getResource(), 0, expected.size());
    commands.transition(gpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->CopyBufferRegion(readback.getResource(), 0, gpu.getResource(), 0, expected.size());
    ASSERT_NO_FATAL_FAILURE(commands.submitAndWait());

    D3D12_RANGE readRange = {0, expected.size()};
    ASSERT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, &readRange, &mapped));
    EXPECT_EQ(std::memcmp(mapped, expected.data(), expected.size()), 0);
    readback.getResource()->Unmap(0, &noRead);
}

TEST_P(DeviceResourceTest, RenderTargetClearReadback)
{
    // Odd dimensions exercise the row pitch returned by GetCopyableFootprints.
    DeviceResource target, readback;
    ASSERT_HRESULT_SUCCEEDED(target.createRenderTarget(device.Get(), 37, 19));
    const auto desc = target.getResource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    ASSERT_HRESULT_SUCCEEDED(readback.createBuffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK));

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ASSERT_HRESULT_SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
    const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.getResource(), nullptr, rtv);

    TestCommands commands;
    ASSERT_NO_FATAL_FAILURE(commands.init(device.Get()));
    commands.transition(target, target.getInitialState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[] = {1.f, 0.f, 1.f, 1.f};
    commands.list->ClearRenderTargetView(rtv, color, 0, nullptr);
    commands.transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = target.getResource();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.getResource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    ASSERT_NO_FATAL_FAILURE(commands.submitAndWait());

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(size)};
    ASSERT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, &readRange, &mapped));
    const unsigned char expected[] = {255, 0, 255, 255};
    for (UINT y = 0; y < desc.Height; ++y)
    {
        const auto* row = static_cast<const unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x)
        {
            EXPECT_EQ(std::memcmp(row + x * 4, expected, 4), 0) << "pixel " << x << ", " << y;
        }
    }
    D3D12_RANGE noWrite = {0, 0};
    readback.getResource()->Unmap(0, &noWrite);
}

TEST_P(DeviceResourceTest, ResourcesKeepTheirDeviceWhenExternalReferenceIsReleased)
{
    DeviceResource first, second;
    ASSERT_HRESULT_SUCCEEDED(first.createRenderTarget(device.Get(), 16, 16));
    ASSERT_HRESULT_SUCCEEDED(second.createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD));
    ID3D12Device* expected = device.Get();
    // Release the diagnostic reference too, so only the resource owners keep the device alive.
    diagnostics.Reset();
    device.Reset();
    expect_owner(first, expected);
    first.reset();
    expect_owner(second, expected);
    // Restore fixture diagnostics before the last resource leaves scope.
    device = second.getDevice();
    device.As(&diagnostics);
}

TEST_P(DeviceResourceTest, ExplicitDevicesDoNotShareResources)
{
    // Hardware + WARP exercises independent devices even on a one-GPU development PC.
    // This is not the physical GPU 1 acceptance test in test_multi_gpu.cpp.
    ComPtr<ID3D12Device> otherDevice = create_test_device(!GetParam());
    if (!otherDevice)
    {
        GTEST_SKIP() << "Both WARP and a hardware device are required for device isolation";
    }
    ASSERT_NE(device.Get(), otherDevice.Get());
    DeviceResource first, second;
    ASSERT_HRESULT_SUCCEEDED(first.createRenderTarget(device.Get(), 16, 16));
    ASSERT_HRESULT_SUCCEEDED(second.createRenderTarget(otherDevice.Get(), 16, 16));
    expect_owner(first, device.Get());
    expect_owner(second, otherDevice.Get());
    first.reset();
    ASSERT_HRESULT_SUCCEEDED(first.createBuffer(otherDevice.Get(), 256, D3D12_HEAP_TYPE_DEFAULT));
    expect_owner(first, otherDevice.Get());
    expect_owner(second, otherDevice.Get());
}

INSTANTIATE_TEST_SUITE_P(NativeDevices, DeviceResourceTest, ::testing::Values(true, false),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Warp" : "Hardware"; });
