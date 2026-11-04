// Copyright 2026 N-GINN LLC. All rights reserved.
#include "device_ssao_pass.h"
#include "framework_ssao_resources.h"

#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <gtest/gtest.h>

#include <cstring>
#include <new>

void* operator new[](size_t size, const char*, int, unsigned, const char*, int) { return ::operator new[](size); }
void* operator new[](size_t size, size_t, size_t, const char*, int, unsigned, const char*, int) { return ::operator new[](size); }

namespace
{
    using Microsoft::WRL::ComPtr;
    using drv3d_dx12::CrossAdapterSyncPoint;

    class Allocator final : public drv3d_dx12::IDeviceResourceAllocator
    {
    public:
        explicit Allocator(ID3D12Device* device) : m_device(device) {}
        ID3D12Device* getDevice() const override { return m_device.Get(); }
        HRESULT allocate(const drv3d_dx12::DeviceResourceRequest& request, ComPtr<ID3D12Resource>& resource, uint64_t& token) override
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = request.heapType;
            token = 0;
            return m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &request.description,
                                                    request.initialState, request.clearValue, IID_PPV_ARGS(&resource));
        }
        void releaseAllocation(uint64_t) override {}
    private:
        ComPtr<ID3D12Device> m_device;
    };

    class DoubleBufferedSsao : public testing::Test
    {
    protected:
        virtual bool frameworkStyle() const { return false; }
        virtual const char* pixelShader() const { return "cbuffer C:register(b0) { float4 color; }; float PS():SV_Target { return color.x; }"; }
        void SetUp() override
        {
            ComPtr<ID3D12Debug> debug;
            ASSERT_HRESULT_SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;
            ASSERT_HRESULT_SUCCEEDED(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> warp;
            ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
            ASSERT_HRESULT_SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            ASSERT_HRESULT_SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
            ASSERT_HRESULT_SUCCEEDED(device.As(&messages));
            ASSERT_HRESULT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&otherGate)));
            static constexpr char vertexShader[] = "float4 VS(uint id:SV_VertexID):SV_Position { return float4(id==1?3:-1,id==2?3:-1,0,1); }";
            ComPtr<ID3DBlob> vertex, pixel, errors;
            ASSERT_HRESULT_SUCCEEDED(D3DCompile(vertexShader, sizeof(vertexShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vertex, &errors));
            ASSERT_HRESULT_SUCCEEDED(D3DCompile(pixelShader(), std::strlen(pixelShader()), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pixel, &errors));
            drv3d_dx12::DeviceSsaoPassDesc desc;
            desc.allocator = eastl::make_shared<Allocator>(device.Get());
            desc.width = desc.height = 4;
            desc.vertexShader = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
            desc.rawShader = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
            desc.frameworkStyle = frameworkStyle();
            ASSERT_HRESULT_SUCCEEDED(pass.init(desc));
        }

        void TearDown() override
        {
            if (gate) gate->Signal(UINT64_MAX - 1);
            if (otherGate) otherGate->Signal(UINT64_MAX - 1);
            EXPECT_HRESULT_SUCCEEDED(pass.reset());
            if (!messages) return;
            for (UINT64 index = 0; index < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
            {
                SIZE_T bytes = 0;
                messages->GetMessage(index, nullptr, &bytes);
                auto storage = eastl::make_unique<uint8_t[]>(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
                ASSERT_HRESULT_SUCCEEDED(messages->GetMessage(index, message, &bytes));
                EXPECT_GT(message->Severity, D3D12_MESSAGE_SEVERITY_WARNING) << message->pDescription;
            }
        }

        DWORD wait(CrossAdapterSyncPoint point, DWORD timeout = 5000)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            EXPECT_NE(event, nullptr);
            EXPECT_HRESULT_SUCCEEDED(point.fence->SetEventOnCompletion(point.value, event));
            const DWORD result = WaitForSingleObject(event, timeout);
            CloseHandle(event);
            return result;
        }

        HRESULT draw(uint32_t index, float color, CrossAdapterSyncPoint inputs = {})
        {
            d3d::mgpu::SsaoConstants constants;
            constants.projection[0] = color;
            return pass.execute(constants, inputs, {}, 0, index);
        }

        void checkOutput(uint32_t index, uint8_t expected)
        {
            const auto desc = pass.getResult(index)->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
            auto allocator = eastl::make_shared<Allocator>(device.Get());
            drv3d_dx12::DeviceResource readback;
            ASSERT_HRESULT_SUCCEEDED(readback.createBuffer(allocator.get(), bytes, D3D12_HEAP_TYPE_READBACK));
            ComPtr<ID3D12CommandQueue> copy;
            D3D12_COMMAND_QUEUE_DESC queue = {};
            queue.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&copy)));
            ComPtr<ID3D12CommandAllocator> commands;
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Fence> done;
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandAllocator(queue.Type, IID_PPV_ARGS(&commands)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateCommandList(0, queue.Type, commands.Get(), nullptr, IID_PPV_ARGS(&list)));
            ASSERT_HRESULT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
            D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
            source.pResource = pass.getResult(index);
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = readback.getResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layout;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            ASSERT_HRESULT_SUCCEEDED(list->Close());
            const auto ready = pass.getReadyPoint(index);
            ASSERT_HRESULT_SUCCEEDED(copy->Wait(ready.fence, ready.value));
            ID3D12CommandList* lists[] = {list.Get()};
            copy->ExecuteCommandLists(1, lists);
            ASSERT_HRESULT_SUCCEEDED(copy->Signal(done.Get(), 1));
            ASSERT_EQ(wait({done.Get(), 1}), WAIT_OBJECT_0);
            void* mapped = nullptr;
            ASSERT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, nullptr, &mapped));
            for (UINT y = 0; y < 4; ++y)
                for (UINT x = 0; x < 4; ++x)
                    EXPECT_NEAR(static_cast<const uint8_t*>(mapped)[layout.Offset + y * layout.Footprint.RowPitch + x], expected, 1);
            readback.getResource()->Unmap(0, nullptr);
        }

        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> messages;
        ComPtr<ID3D12Fence> gate, otherGate;
        drv3d_dx12::DeviceSsaoPass pass;
    };

    TEST_F(DoubleBufferedSsao, InputCopyForOtherSetDoesNotWaitForBlockedDraw)
    {
        ASSERT_NE(pass.getDepth(0), pass.getDepth(1));
        ASSERT_NE(pass.getNormals(0), pass.getNormals(1));
        ASSERT_NE(pass.getResult(0), pass.getResult(1));
        ASSERT_HRESULT_SUCCEEDED(draw(0, 0.25f, {gate.Get(), 1}));
        const auto free = pass.getReadyPoint(1);
        EXPECT_EQ(free.value, 0u);
        // This is the same dependency the input-copy queue uses before overwriting its set.
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> copy;
        ASSERT_HRESULT_SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&copy)));
        if (free.fence) ASSERT_HRESULT_SUCCEEDED(copy->Wait(free.fence, free.value));
        ASSERT_HRESULT_SUCCEEDED(copy->Signal(otherGate.Get(), 1));
        EXPECT_EQ(wait({otherGate.Get(), 1}), WAIT_OBJECT_0);
        EXPECT_LT(pass.getReadyPoint(0).fence->GetCompletedValue(), pass.getReadyPoint(0).value);
        ASSERT_HRESULT_SUCCEEDED(draw(1, 0.75f, {gate.Get(), 1}));
        ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
        checkOutput(0, 64);
        checkOutput(1, 191);
    }

    TEST_F(DoubleBufferedSsao, OutputReaderBlocksOnlyReuseOfItsOwnSet)
    {
        ASSERT_HRESULT_SUCCEEDED(draw(0, 0.25f));
        ASSERT_EQ(wait(pass.getReadyPoint(0)), WAIT_OBJECT_0);
        ASSERT_HRESULT_SUCCEEDED(pass.releaseOutput({gate.Get(), 1}, 0));
        ASSERT_HRESULT_SUCCEEDED(draw(1, 0.75f));
        ASSERT_EQ(wait(pass.getReadyPoint(1)), WAIT_OBJECT_0);
        checkOutput(0, 64);
        checkOutput(1, 191);
        ASSERT_HRESULT_SUCCEEDED(draw(0, 0.5f));
        EXPECT_EQ(wait(pass.getReadyPoint(0), 100), WAIT_TIMEOUT);
        ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
        checkOutput(0, 128);
        checkOutput(1, 191);
    }

    TEST_F(DoubleBufferedSsao, ResetDrainsDeferredReadersOnBothSets)
    {
        ASSERT_HRESULT_SUCCEEDED(draw(0, 0.25f));
        ASSERT_HRESULT_SUCCEEDED(draw(1, 0.75f));
        ASSERT_EQ(wait(pass.getReadyPoint(1)), WAIT_OBJECT_0);
        ASSERT_HRESULT_SUCCEEDED(pass.releaseOutput({gate.Get(), 1}, 0));
        ASSERT_HRESULT_SUCCEEDED(pass.releaseOutput({otherGate.Get(), 1}, 1));
        EXPECT_EQ(pass.reset(0), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
        EXPECT_EQ(pass.reset(0), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        ASSERT_HRESULT_SUCCEEDED(otherGate->Signal(1));
        EXPECT_HRESULT_SUCCEEDED(pass.reset());
        EXPECT_EQ(pass.getDepth(0), nullptr);
        EXPECT_EQ(pass.getDepth(1), nullptr);
    }

    class FrameworkSsao : public DoubleBufferedSsao
    {
    protected:
        bool frameworkStyle() const override { return true; }
        const char* pixelShader() const override
        {
            return "Texture2D<float> depth:register(t1); float PS(float4 p:SV_Position):SV_Target { return depth.Load(int3(int2(p.xy),0)); }";
        }
        void SetUp() override
        {
            DoubleBufferedSsao::SetUp();
            if (HasFatalFailure()) return;
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> warp;
            ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
            ASSERT_HRESULT_SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            ASSERT_HRESULT_SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primary)));
            drv3d_dx12::DeviceSsaoPassDesc desc;
            desc.width = desc.height = 4;
            ASSERT_HRESULT_SUCCEEDED(shared.init(primary.Get(), device.Get(), desc));
        }

        void writeDepth(float value)
        {
            auto* texture = shared.primary().depth;
            const auto desc = texture->GetDesc();
            EXPECT_EQ(desc.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
            EXPECT_EQ(desc.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
            UINT64 bytes = 0;
            primary->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
            auto allocator = eastl::make_shared<Allocator>(primary.Get());
            drv3d_dx12::DeviceResource upload;
            ASSERT_HRESULT_SUCCEEDED(upload.createBuffer(allocator.get(), bytes, D3D12_HEAP_TYPE_UPLOAD));
            void* mapped = nullptr;
            ASSERT_HRESULT_SUCCEEDED(upload.getResource()->Map(0, nullptr, &mapped));
            for (UINT y = 0; y < 4; ++y)
                for (UINT x = 0; x < 4; ++x)
                    std::memcpy(static_cast<uint8_t*>(mapped) + layout.Offset + y * layout.Footprint.RowPitch + x * sizeof(float), &value, sizeof(value));
            upload.getResource()->Unmap(0, nullptr);
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12CommandAllocator> commands;
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Fence> done;
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ASSERT_HRESULT_SUCCEEDED(primary->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
            ASSERT_HRESULT_SUCCEEDED(primary->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&commands)));
            ASSERT_HRESULT_SUCCEEDED(primary->CreateCommandList(0, queueDesc.Type, commands.Get(), nullptr, IID_PPV_ARGS(&list)));
            ASSERT_HRESULT_SUCCEEDED(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
            source.pResource = upload.getResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layout;
            destination.pResource = texture;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            list->ResourceBarrier(1, &barrier);
            ASSERT_HRESULT_SUCCEEDED(list->Close());
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            ASSERT_HRESULT_SUCCEEDED(queue->Signal(done.Get(), 1));
            ASSERT_EQ(wait({done.Get(), 1}), WAIT_OBJECT_0);
        }

        ComPtr<ID3D12Device> primary;
        drv3d_dx12::FrameworkSsaoResources shared;
    };

    TEST_F(FrameworkSsao, ReadsSharedInputAtExecutionRatherThanSubmission)
    {
        writeDepth(0.25f);
        ASSERT_HRESULT_SUCCEEDED(pass.getQueue()->Wait(gate.Get(), 1)); // Local test gate, not part of the transport.
        d3d::mgpu::SsaoConstants constants;
        ASSERT_EQ(pass.executeFramework(constants, shared.secondary(), 1), S_OK);
        writeDepth(0.75f); // Change shared input after the task has already been submitted.
        ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
        checkOutput(0, 191); // The task must consume the newer shared input.
        EXPECT_EQ(pass.getDepth(1), nullptr);
        EXPECT_EQ(pass.getResult(1), nullptr);
        EXPECT_EQ(pass.releaseOutput({otherGate.Get(), 1}), DXGI_ERROR_INVALID_CALL);
    }

    TEST_F(FrameworkSsao, ThreeCommandRecordingsBoundOnlyGpu1Work)
    {
        writeDepth(0.5f);
        ASSERT_HRESULT_SUCCEEDED(pass.getQueue()->Wait(gate.Get(), 1));
        d3d::mgpu::SsaoConstants constants;
        for (uint64_t frame = 1; frame <= 3; ++frame)
            ASSERT_EQ(pass.executeFramework(constants, shared.secondary(), frame), S_OK);
        EXPECT_EQ(pass.executeFramework(constants, shared.secondary(), 4), S_FALSE);
        ASSERT_HRESULT_SUCCEEDED(gate->Signal(1));
        ASSERT_EQ(wait(pass.getReadyPoint()), WAIT_OBJECT_0);
        EXPECT_EQ(pass.executeFramework(constants, shared.secondary(), 5), S_OK);
        checkOutput(0, 128);
    }
}
