// Copyright 2026 N-GINN LLC. All rights reserved.

#include <EASTL/array.h>
#include <EASTL/unique_ptr.h>
#include <gtest/gtest.h>

#include <cstring>

#include "device_resource.h"
#include "multi_gpu_device.h"
#include "nau/directx/d3d12sdklayers.h"

namespace
{
    using drv3d_dx12::DeviceResource;
    using Microsoft::WRL::ComPtr;
    /**
     * @brief Checks both the explicit owner and the resource's native COM device identity.
     */
    void expectOwner(const DeviceResource& resource, ID3D12Device* device)
    {
        ASSERT_NE(resource.getResource(), nullptr);
        EXPECT_EQ(resource.getDevice(), device);
        ComPtr<ID3D12Device> actual;
        ASSERT_HRESULT_SUCCEEDED(resource.getResource()->GetDevice(IID_PPV_ARGS(&actual)));
        EXPECT_EQ(actual.Get(), device);
    }

    /**
     * @brief Creates an independent WARP or hardware device for resource tests.
     */
    ComPtr<ID3D12Device> createTestDevice(bool warp)
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
    public:
        DeviceResourceTest() = default;
        DeviceResourceTest(const DeviceResourceTest&) = delete;
        DeviceResourceTest(DeviceResourceTest&&) = delete;
        DeviceResourceTest& operator=(const DeviceResourceTest&) = delete;
        DeviceResourceTest& operator=(DeviceResourceTest&&) = delete;
        ~DeviceResourceTest() override = default;

    protected:
        void SetUp() override
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
            }
            m_device = createTestDevice(GetParam());
            if (!m_device)
            {
                GTEST_SKIP() << (GetParam() ? "D3D12 WARP unavailable" : "D3D12 hardware unavailable");
            }
            if (debug)
            {
                ASSERT_HRESULT_SUCCEEDED(m_device.As(&m_diagnostics));
                m_diagnostics->ClearStoredMessages();
            }
        }

        void TearDown() override
        {
            if (!m_diagnostics)
            {
                return;
            }
            // Test-local resources and command objects have already been destroyed here.
            ComPtr<ID3D12DebugDevice> debugDevice;
            ASSERT_HRESULT_SUCCEEDED(m_device.As(&debugDevice));
            EXPECT_HRESULT_SUCCEEDED(debugDevice->ReportLiveDeviceObjects(
                static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)));
            for (UINT64 index = 0; index < m_diagnostics->GetNumStoredMessages(); ++index)
            {
                SIZE_T size = 0;
                ASSERT_HRESULT_SUCCEEDED(m_diagnostics->GetMessage(index, nullptr, &size));
                eastl::unique_ptr<char[]> storage(new char[size]);
                D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
                ASSERT_HRESULT_SUCCEEDED(m_diagnostics->GetMessage(index, message, &size));
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
                EXPECT_NE(message->ID, D3D12_MESSAGE_ID_LIVE_RESOURCE) << message->pDescription;
                EXPECT_NE(message->ID, D3D12_MESSAGE_ID_LIVE_HEAP) << message->pDescription;
            }
        }

    protected:
        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12InfoQueue> m_diagnostics;
    };

    /**
     * @brief Records and synchronously submits commands only for tests.
     *
     * Production resources do not own a queue or hide a GPU wait.
     */
    class TestCommands
    {
    public:
        TestCommands() = default;
        TestCommands(const TestCommands&) = delete;
        TestCommands(TestCommands&&) = delete;
        TestCommands& operator=(const TestCommands&) = delete;
        TestCommands& operator=(TestCommands&&) = delete;
        ~TestCommands() = default;

    public:
        /**
         * @brief Creates command objects on the resource owner's device.
         */
        void init(ID3D12Device* device)
        {
            D3D12_COMMAND_QUEUE_DESC desc = {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&m_allocator)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandList(0, desc.Type, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_list)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        }

        /**
         * @brief Records a whole-resource transition on this command list.
         */
        void transition(const DeviceResource& resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource.getResource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            m_list->ResourceBarrier(1, &barrier);
        }

        /**
         * @brief Submits the recorded commands and waits for their completion before returning.
         */
        void submitAndWait()
        {
            ASSERT_HRESULT_SUCCEEDED(m_list->Close());
            ID3D12CommandList* lists[] = {m_list.Get()};
            m_queue->ExecuteCommandLists(1, lists);
            ASSERT_HRESULT_SUCCEEDED(m_queue->Signal(m_fence.Get(), 1));
            // A null event blocks until completion; CTest's timeout bounds a hung driver.
            ASSERT_HRESULT_SUCCEEDED(m_fence->SetEventOnCompletion(1, nullptr));
            ASSERT_EQ(m_fence->GetCompletedValue(), 1u);
        }

        /**
         * @brief Returns the borrowed command list for test-specific copy and clear operations.
         */
        ID3D12GraphicsCommandList* getList() const
        {
            return m_list.Get();
        }

    private:
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList> m_list;
        ComPtr<ID3D12Fence> m_fence;
    };

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
        EXPECT_EQ(resource.createRenderTarget(m_device.Get(), 0, 16), E_INVALIDARG);
        EXPECT_EQ(resource.createRenderTarget(m_device.Get(), 16, 0), E_INVALIDARG);
        EXPECT_EQ(resource.createRenderTarget(m_device.Get(), D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1, 16), E_INVALIDARG);
        EXPECT_EQ(resource.createRenderTarget(m_device.Get(), 16, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1), E_INVALIDARG);
        EXPECT_EQ(resource.createBuffer(m_device.Get(), 0, D3D12_HEAP_TYPE_DEFAULT), E_INVALIDARG);
        EXPECT_EQ(resource.createBuffer(m_device.Get(), 256, D3D12_HEAP_TYPE_CUSTOM), E_INVALIDARG);
        EXPECT_EQ(resource.getResource(), nullptr);
        EXPECT_EQ(resource.getDevice(), nullptr);
    }

    TEST_P(DeviceResourceTest, PreservesOwnerUntilExplicitReset)
    {
        DeviceResource resource;
        ASSERT_HRESULT_SUCCEEDED(resource.createRenderTarget(m_device.Get(), 37, 19));
        expectOwner(resource, m_device.Get());
        ID3D12Resource* original = resource.getResource();
        const D3D12_RESOURCE_DESC desc = original->GetDesc();
        EXPECT_EQ(desc.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        EXPECT_EQ(desc.Width, 37u);
        EXPECT_EQ(desc.Height, 19u);
        EXPECT_EQ(desc.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
        EXPECT_EQ(desc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        EXPECT_EQ(resource.getInitialState(), D3D12_RESOURCE_STATE_COMMON);

        EXPECT_EQ(resource.createBuffer(m_device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD), DXGI_ERROR_INVALID_CALL);
        EXPECT_EQ(resource.createRenderTarget(nullptr, 16, 16), E_INVALIDARG);
        EXPECT_EQ(resource.getResource(), original);
        expectOwner(resource, m_device.Get());
        resource.reset();
        EXPECT_EQ(resource.getResource(), nullptr);
        EXPECT_EQ(resource.getDevice(), nullptr);
        for (D3D12_HEAP_TYPE heapType : {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK})
        {
            ASSERT_HRESULT_SUCCEEDED(resource.createBuffer(m_device.Get(), 1024, heapType));
            expectOwner(resource, m_device.Get());
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
        eastl::array<unsigned char, 1024> expected;
        for (size_t index = 0; index < expected.size(); ++index)
        {
            expected[index] = static_cast<unsigned char>(index * 17 + 3);
        }
        DeviceResource upload;
        DeviceResource gpu;
        DeviceResource readback;
        ASSERT_HRESULT_SUCCEEDED(upload.createBuffer(m_device.Get(), expected.size(), D3D12_HEAP_TYPE_UPLOAD));
        ASSERT_HRESULT_SUCCEEDED(gpu.createBuffer(m_device.Get(), expected.size(), D3D12_HEAP_TYPE_DEFAULT));
        ASSERT_HRESULT_SUCCEEDED(readback.createBuffer(m_device.Get(), expected.size(), D3D12_HEAP_TYPE_READBACK));
        EXPECT_EQ(upload.getInitialState(), D3D12_RESOURCE_STATE_GENERIC_READ);
        EXPECT_EQ(readback.getInitialState(), D3D12_RESOURCE_STATE_COPY_DEST);
        void* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        ASSERT_HRESULT_SUCCEEDED(upload.getResource()->Map(0, &noRead, &mapped));
        std::memcpy(mapped, expected.data(), expected.size());
        upload.getResource()->Unmap(0, nullptr);

        TestCommands commands;
        ASSERT_NO_FATAL_FAILURE(commands.init(m_device.Get()));
        commands.transition(gpu, gpu.getInitialState(), D3D12_RESOURCE_STATE_COPY_DEST);
        commands.getList()->CopyBufferRegion(gpu.getResource(), 0, upload.getResource(), 0, expected.size());
        commands.transition(gpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands.getList()->CopyBufferRegion(readback.getResource(), 0, gpu.getResource(), 0, expected.size());
        ASSERT_NO_FATAL_FAILURE(commands.submitAndWait());

        D3D12_RANGE readRange = {0, expected.size()};
        ASSERT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, &readRange, &mapped));
        EXPECT_EQ(std::memcmp(mapped, expected.data(), expected.size()), 0);
        readback.getResource()->Unmap(0, &noRead);
    }

    TEST_P(DeviceResourceTest, RenderTargetClearReadback)
    {
        // Odd dimensions exercise the row pitch returned by GetCopyableFootprints.
        DeviceResource target;
        DeviceResource readback;
        ASSERT_HRESULT_SUCCEEDED(target.createRenderTarget(m_device.Get(), 37, 19));
        const D3D12_RESOURCE_DESC desc = target.getResource()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 size = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        ASSERT_HRESULT_SUCCEEDED(readback.createBuffer(m_device.Get(), size, D3D12_HEAP_TYPE_READBACK));

        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateRenderTargetView(target.getResource(), nullptr, rtv);

        TestCommands commands;
        ASSERT_NO_FATAL_FAILURE(commands.init(m_device.Get()));
        commands.transition(target, target.getInitialState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        constexpr float CLEAR_COLOR[] = {1.f, 0.f, 1.f, 1.f};
        commands.getList()->ClearRenderTargetView(rtv, CLEAR_COLOR, 0, nullptr);
        commands.transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = target.getResource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.getResource();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commands.getList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        ASSERT_NO_FATAL_FAILURE(commands.submitAndWait());

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, static_cast<SIZE_T>(size)};
        ASSERT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, &readRange, &mapped));
        constexpr unsigned char EXPECTED_PIXEL[] = {255, 0, 255, 255};
        for (UINT y = 0; y < desc.Height; ++y)
        {
            const unsigned char* row = static_cast<const unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x)
            {
                EXPECT_EQ(std::memcmp(row + x * 4, EXPECTED_PIXEL, 4), 0) << "pixel " << x << ", " << y;
            }
        }
        D3D12_RANGE noWrite = {0, 0};
        readback.getResource()->Unmap(0, &noWrite);
    }

    TEST_P(DeviceResourceTest, ResourcesKeepTheirDeviceWhenExternalReferenceIsReleased)
    {
        DeviceResource first;
        DeviceResource second;
        ASSERT_HRESULT_SUCCEEDED(first.createRenderTarget(m_device.Get(), 16, 16));
        ASSERT_HRESULT_SUCCEEDED(second.createBuffer(m_device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD));
        ID3D12Device* expected = m_device.Get();
        // Release the diagnostic reference too, so only the resource owners keep the device alive.
        m_diagnostics.Reset();
        m_device.Reset();
        expectOwner(first, expected);
        first.reset();
        expectOwner(second, expected);
        // Restore fixture diagnostics before the last resource leaves scope.
        m_device = second.getDevice();
        m_device.As(&m_diagnostics);
    }

    TEST_P(DeviceResourceTest, ExplicitDevicesDoNotShareResources)
    {
        // Hardware + WARP exercises independent devices even on a one-GPU development PC.
        // This is not the physical GPU 1 acceptance test in test_multi_gpu.cpp.
        ComPtr<ID3D12Device> otherDevice = createTestDevice(!GetParam());
        if (!otherDevice)
        {
            GTEST_SKIP() << "Both WARP and a hardware device are required for device isolation";
        }
        ASSERT_NE(m_device.Get(), otherDevice.Get());
        DeviceResource first;
        DeviceResource second;
        ASSERT_HRESULT_SUCCEEDED(first.createRenderTarget(m_device.Get(), 16, 16));
        ASSERT_HRESULT_SUCCEEDED(second.createRenderTarget(otherDevice.Get(), 16, 16));
        expectOwner(first, m_device.Get());
        expectOwner(second, otherDevice.Get());
        first.reset();
        ASSERT_HRESULT_SUCCEEDED(first.createBuffer(otherDevice.Get(), 256, D3D12_HEAP_TYPE_DEFAULT));
        expectOwner(first, otherDevice.Get());
        expectOwner(second, otherDevice.Get());
    }

    INSTANTIATE_TEST_SUITE_P(NativeDevices, DeviceResourceTest, ::testing::Values(true, false), [](const ::testing::TestParamInfo<bool>& info)
    {
        return info.param ? "Warp" : "Hardware";
    });

}  // namespace
