// test_device_ssao_pass.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include <cstdlib>

// allocator_malloc requires the C allocation declarations before its header.
#include <EASTL/allocator_malloc.h>
#include <EASTL/vector.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "device_ssao_pass.h"
#include "nau/directx/d3d12sdklayers.h"
#include "test_resource_allocator.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    template <typename TValue>
    using TestVector = eastl::vector<TValue, eastl::allocator_malloc>;
    constexpr UINT WIDTH = 79;
    constexpr UINT HEIGHT = 53;

    class SsaoInclude final : public ID3DInclude
    {
    public:
        SsaoInclude() = default;
        SsaoInclude(const SsaoInclude&) = delete;
        SsaoInclude(SsaoInclude&&) = delete;
        SsaoInclude& operator=(const SsaoInclude&) = delete;
        SsaoInclude& operator=(SsaoInclude&&) = delete;
        ~SsaoInclude() = default;

        HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
        {
            if (std::strcmp(name, "ssao_common.hlsli") != 0)
            {
                return E_INVALIDARG;
            }
            FILE* file = nullptr;
            if (fopen_s(&file, NAU_SSAO_SHADER_DIR "/include/ssao_common.hlsli", "rb") != 0)
            {
                return E_FAIL;
            }
            fseek(file, 0, SEEK_END);
            m_text.resize(static_cast<size_t>(ftell(file)));
            rewind(file);
            const size_t bytes = fread(m_text.data(), 1, m_text.size(), file);
            fclose(file);
            *data = m_text.data();
            *size = static_cast<UINT>(bytes);
            return bytes == m_text.size() ? S_OK : E_FAIL;
        }
        HRESULT Close(LPCVOID) override
        {
            return S_OK;
        }

    private:
        TestVector<char> m_text;
    };

    class RetirementProfiler final : public d3d::mgpu::ISsaoProfiler
    {
    public:
        explicit RetirementProfiler(bool& detached) :
            m_detached(detached)
        {
        }
        RetirementProfiler(const RetirementProfiler&) = delete;
        RetirementProfiler(RetirementProfiler&&) = delete;
        RetirementProfiler& operator=(const RetirementProfiler&) = delete;
        RetirementProfiler& operator=(RetirementProfiler&&) = delete;
        ~RetirementProfiler() override = default;

        void attach(void*, void*) override
        {
        }
        void beginPass(void*, const char*) override
        {
        }
        void endPass() override
        {
        }
        void resolve(void*) override
        {
        }
        void submitted() override
        {
        }
        void detach() override
        {
            m_detached = true;
        }
        eastl::shared_ptr<d3d::mgpu::ITransferProfiler> createTransferProfiler(const char*, bool) override
        {
            return {};
        }

    private:
        bool& m_detached;
    };

    class DeviceSsao : public ::testing::Test
    {
    public:
        DeviceSsao() = default;
        DeviceSsao(const DeviceSsao&) = delete;
        DeviceSsao(DeviceSsao&&) = delete;
        DeviceSsao& operator=(const DeviceSsao&) = delete;
        DeviceSsao& operator=(DeviceSsao&&) = delete;
        ~DeviceSsao() override = default;

    protected:
        void SetUp() override
        {
            ComPtr<ID3D12Debug> debug;
            ASSERT_HRESULT_SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter1> warp;
            ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
            ASSERT_HRESULT_SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            ASSERT_HRESULT_SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
            ASSERT_HRESULT_SUCCEEDED(m_device.As(&m_diagnostics));
            m_diagnostics->ClearStoredMessages();
            m_allocator = mgpu_test::makeTestResourceAllocator();
            ASSERT_HRESULT_SUCCEEDED(m_allocator->init(m_device.Get()));
            const auto compile = [](const wchar_t* source, const char* entry, const char* profile)
            {
                wchar_t path[1024] = {};
                swprintf_s(path, L"%hs/src/%ls", NAU_SSAO_SHADER_DIR, source);
                ComPtr<ID3DBlob> code;
                ComPtr<ID3DBlob> errors;
                SsaoInclude includes;
                const HRESULT result = D3DCompileFromFile(path, nullptr, &includes, entry, profile,
                                                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
                EXPECT_HRESULT_SUCCEEDED(result) << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "No diagnostics");
                return code;
            };
            m_vertex = compile(L"ssao.hlsl", "VSMain", "vs_5_0");
            m_raw = compile(L"ssao.hlsl", "PSMain", "ps_5_0");
            m_blur = compile(L"ssao_blur.hlsl", "PSMain", "ps_5_0");
            m_normal = compile(L"ssao_blur.hlsl", "PSReconstructNormals", "ps_5_0");
            ASSERT_TRUE(m_vertex && m_raw && m_blur && m_normal);
            drv3d_dx12::DeviceSsaoPassDesc desc;
            desc.allocator = m_allocator;
            desc.width = WIDTH;
            desc.height = HEIGHT;
            desc.vertexShader = {m_vertex->GetBufferPointer(), m_vertex->GetBufferSize()};
            desc.rawShader = {m_raw->GetBufferPointer(), m_raw->GetBufferSize()};
            desc.blurShader = {m_blur->GetBufferPointer(), m_blur->GetBufferSize()};
            ASSERT_HRESULT_SUCCEEDED(m_pass.init(desc));
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ASSERT_HRESULT_SUCCEEDED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue)));
            ASSERT_HRESULT_SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_copyAllocator)));
            ASSERT_HRESULT_SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_copyAllocator.Get(), nullptr, IID_PPV_ARGS(&m_copyList)));
            ASSERT_HRESULT_SUCCEEDED(m_copyList->Close());
            ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence)));
            constexpr float PROJECTION_A = 0.1f / 99.9f;
            constexpr float PROJECTION_B = 10.0f / 99.9f;
            const float projection[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, PROJECTION_A, -1, 0, 0, PROJECTION_B, 0};
            const float inverse[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 / PROJECTION_B, 0, 0, -1, PROJECTION_A / PROJECTION_B};
            const float view[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(m_constants.projection, projection, sizeof(projection));
            std::memcpy(m_constants.inverseProjection, inverse, sizeof(inverse));
            std::memcpy(m_constants.view, view, sizeof(view));
        }

        void TearDown() override
        {
            EXPECT_HRESULT_SUCCEEDED(m_pass.reset());
            EXPECT_EQ(drv3d_dx12::DeviceSsaoPass::collectRetiredResources(), 0u);
            if (m_allocator)
            {
                EXPECT_EQ(m_allocator->getAllocationCount(), 0u);
                EXPECT_HRESULT_SUCCEEDED(m_allocator->shutdown());
            }
            if (!m_diagnostics)
            {
                return;
            }
            for (UINT64 index = 0; index < m_diagnostics->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0;
                ASSERT_HRESULT_SUCCEEDED(m_diagnostics->GetMessage(index, nullptr, &bytes));
                TestVector<char> storage(bytes);
                D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                ASSERT_HRESULT_SUCCEEDED(m_diagnostics->GetMessage(index, message, &bytes));
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
            }
        }

        void wait(drv3d_dx12::CrossAdapterSyncPoint point)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ASSERT_NE(event, nullptr);
            EXPECT_HRESULT_SUCCEEDED(point.fence->SetEventOnCompletion(point.value, event));
            EXPECT_EQ(WaitForSingleObject(event, 10000), WAIT_OBJECT_0);
            CloseHandle(event);
        }

        void submitCopy()
        {
            ASSERT_HRESULT_SUCCEEDED(m_copyList->Close());
            ID3D12CommandList* lists[] = {m_copyList.Get()};
            m_copyQueue->ExecuteCommandLists(1, lists);
            ASSERT_HRESULT_SUCCEEDED(m_copyQueue->Signal(m_copyFence.Get(), ++m_copyValue));
            wait({m_copyFence.Get(), m_copyValue});
        }

        void fillInputs(bool contact, bool background = false, drv3d_dx12::DeviceSsaoPass* target = nullptr, bool poisonNormals = false)
        {
            drv3d_dx12::DeviceSsaoPass& pass = target ? *target : m_pass;
            wait(pass.getReadyPoint());
            for (UINT input = 0; input < 2; ++input)
            {
                ID3D12Resource* texture = input == 0 ? pass.getNormals() : pass.getDepth();
                const D3D12_RESOURCE_DESC desc = texture->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
                UINT64 bytes = 0;
                m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
                drv3d_dx12::DeviceResource upload;
                ASSERT_HRESULT_SUCCEEDED(upload.createBuffer(m_allocator.get(), bytes, D3D12_HEAP_TYPE_UPLOAD));
                void* data = nullptr;
                ASSERT_HRESULT_SUCCEEDED(upload.getResource()->Map(0, nullptr, &data));
                for (UINT y = 0; y < HEIGHT; ++y)
                {
                    float* row = reinterpret_cast<float*>(static_cast<uint8_t*>(data) + y * footprint.Footprint.RowPitch);
                    for (UINT x = 0; x < WIDTH; ++x)
                    {
                        if (input == 0)
                        {
                            uint16_t* packed = reinterpret_cast<uint16_t*>(row);
                            packed[x * 2] = packed[x * 2 + 1] = poisonNormals ? 0 : 0x3800;  // float16(0.5), sphere-map +Z.
                        }
                        else
                        {
                            const float viewZ = contact && x < WIDTH / 2 ? -2.7f : -3.0f;
                            row[x] = background ? 0.0f : ((0.1f / 99.9f) * viewZ + 10.0f / 99.9f) / -viewZ;
                        }
                    }
                }
                upload.getResource()->Unmap(0, nullptr);
                ASSERT_HRESULT_SUCCEEDED(m_copyAllocator->Reset());
                ASSERT_HRESULT_SUCCEEDED(m_copyList->Reset(m_copyAllocator.Get(), nullptr));
                D3D12_RESOURCE_BARRIER transition = {};
                transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                transition.Transition = {texture, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
                m_copyList->ResourceBarrier(1, &transition);
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = upload.getResource();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = texture;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                m_copyList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                m_copyList->ResourceBarrier(1, &transition);
                submitCopy();
            }
        }

        TestVector<float> read(bool raw = false)
        {
            wait(m_pass.getReadyPoint());
            ID3D12Resource* texture = m_pass.getResult(raw);
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            UINT64 bytes = 0;
            m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            drv3d_dx12::DeviceResource readback;
            EXPECT_HRESULT_SUCCEEDED(readback.createBuffer(m_allocator.get(), bytes, D3D12_HEAP_TYPE_READBACK));
            EXPECT_HRESULT_SUCCEEDED(m_copyAllocator->Reset());
            EXPECT_HRESULT_SUCCEEDED(m_copyList->Reset(m_copyAllocator.Get(), nullptr));
            D3D12_RESOURCE_BARRIER transition = {};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition = {texture, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
            m_copyList->ResourceBarrier(1, &transition);
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = readback.getResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            m_copyList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            m_copyList->ResourceBarrier(1, &transition);
            submitCopy();
            void* data = nullptr;
            EXPECT_HRESULT_SUCCEEDED(readback.getResource()->Map(0, nullptr, &data));
            TestVector<float> result(WIDTH * HEIGHT);
            for (UINT y = 0; y < HEIGHT; ++y)
            {
                const uint8_t* row = static_cast<uint8_t*>(data) + y * footprint.Footprint.RowPitch;
                if (desc.Format == DXGI_FORMAT_R8_UNORM)
                {
                    for (UINT x = 0; x < WIDTH; ++x)
                    {
                        result[y * WIDTH + x] = row[x] / 255.0f;
                    }
                }
                else
                {
                    std::memcpy(result.data() + y * WIDTH, row, WIDTH * sizeof(float));
                }
            }
            readback.getResource()->Unmap(0, nullptr);
            return result;
        }

        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12InfoQueue> m_diagnostics;
        eastl::shared_ptr<mgpu_test::TestResourceAllocator> m_allocator;
        drv3d_dx12::DeviceSsaoPass m_pass;
        ComPtr<ID3DBlob> m_vertex, m_raw, m_blur, m_normal;
        ComPtr<ID3D12CommandQueue> m_copyQueue;
        ComPtr<ID3D12CommandAllocator> m_copyAllocator;
        ComPtr<ID3D12GraphicsCommandList> m_copyList;
        ComPtr<ID3D12Fence> m_copyFence;
        UINT64 m_copyValue = 0;
        d3d::mgpu::SsaoConstants m_constants;
    };

    TEST_F(DeviceSsao, BackgroundAndFlatSurfaceAreNeutral)
    {
        for (bool background : {true, false})
        {
            fillInputs(false, background);
            ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true));
            const TestVector<float> values = read();
            for (float value : values)
            {
                EXPECT_NEAR(value, 1.0f, 0.000001f);
            }
        }
    }

    TEST_F(DeviceSsao, ContactShadowAndBothBlurPassesRun)
    {
        fillInputs(true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true));
        const TestVector<float> filtered = read();
        const TestVector<float> raw = read(true);
        UINT changed = 0;
        UINT occluded = 0;
        for (UINT pixel = 0; pixel < filtered.size(); ++pixel)
        {
            EXPECT_TRUE(std::isfinite(filtered[pixel]));
            EXPECT_GE(filtered[pixel], 0.0f);
            EXPECT_LE(filtered[pixel], 1.000001f);
            changed += std::abs(filtered[pixel] - raw[pixel]) > 0.0001f ? 1 : 0;
            occluded += filtered[pixel] < 0.95f ? 1 : 0;
        }
        EXPECT_GT(changed, 10u);
        EXPECT_GT(occluded, 10u);
    }

    TEST_F(DeviceSsao, CompactTransportPreservesRawPrecisionAndBoundsOutputQuantization)
    {
        EXPECT_EQ(m_pass.getNormals()->GetDesc().Format, DXGI_FORMAT_R16G16_FLOAT);
        EXPECT_EQ(m_pass.getResult()->GetDesc().Format, DXGI_FORMAT_R8_UNORM);
        EXPECT_EQ(m_pass.getResult(true)->GetDesc().Format, DXGI_FORMAT_R32_FLOAT);
        fillInputs(true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, false));
        const TestVector<float> raw = read(true);
        const TestVector<float> result = read();
        for (UINT pixel = 0; pixel < result.size(); ++pixel)
        {
            EXPECT_LE(std::abs(result[pixel] - raw[pixel]), 0.5f / 255.0f + 0.000001f);
            EXPECT_NEAR(result[pixel] * 255.0f, std::round(result[pixel] * 255.0f), 0.00001f);
        }
    }

    TEST_F(DeviceSsao, ReconstructsNormalsOnItsOwnQueueFromDepthWithoutNormalInput)
    {
        fillInputs(true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true));
        const TestVector<float> expected = read();
        const TestVector<float> expectedRaw = read(true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.reset());
        drv3d_dx12::DeviceSsaoPassDesc desc;
        desc.allocator = m_allocator;
        desc.width = WIDTH;
        desc.height = HEIGHT;
        desc.vertexShader = {m_vertex->GetBufferPointer(), m_vertex->GetBufferSize()};
        desc.rawShader = {m_raw->GetBufferPointer(), m_raw->GetBufferSize()};
        desc.blurShader = {m_blur->GetBufferPointer(), m_blur->GetBufferSize()};
        desc.normalShader = {m_normal->GetBufferPointer(), m_normal->GetBufferSize()};
        ASSERT_HRESULT_SUCCEEDED(m_pass.init(desc));
        fillInputs(true, false, nullptr, true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true));
        const TestVector<float> actual = read();
        const TestVector<float> actualRaw = read(true);
        for (UINT pixel = 0; pixel < actual.size(); ++pixel)
        {
            ASSERT_NEAR(actual[pixel], expected[pixel], 1.0f / 255.0f);
            ASSERT_NEAR(actualRaw[pixel], expectedRaw[pixel], 0.0001f);
        }
        fillInputs(false, true, nullptr, true);
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true));
        for (float value : read())
        {
            ASSERT_NEAR(value, 1.0f, 0.000001f);
        }
    }

    TEST_F(DeviceSsao, InFlightConstantSlotsReturnBusyInsteadOfBlocking)
    {
        fillInputs(false);
        ComPtr<ID3D12Fence> gate;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true, {gate.Get(), 1}));
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true, {gate.Get(), 1}));
        EXPECT_EQ(m_pass.canSubmit(), DXGI_ERROR_WAS_STILL_DRAWING);
        EXPECT_EQ(m_pass.execute(m_constants, true, {gate.Get(), 1}), DXGI_ERROR_WAS_STILL_DRAWING);
        EXPECT_HRESULT_SUCCEEDED(gate->Signal(1));
        wait(m_pass.getReadyPoint());
        EXPECT_EQ(m_pass.canSubmit(), S_OK);
    }

    TEST_F(DeviceSsao, PendingInputWaitRetainsResourcesUntilItsFenceCompletes)
    {
        fillInputs(false);
        ComPtr<ID3D12Fence> gate;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        ASSERT_HRESULT_SUCCEEDED(m_pass.execute(m_constants, true, {gate.Get(), 1}));
        EXPECT_EQ(m_pass.reset(0), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        EXPECT_GT(m_allocator->getAllocationCount(), 0u);
        EXPECT_HRESULT_SUCCEEDED(gate->Signal(1));
        wait(m_pass.getReadyPoint());
        EXPECT_HRESULT_SUCCEEDED(m_pass.reset());
        EXPECT_EQ(m_allocator->getAllocationCount(), 0u);
    }

    TEST_F(DeviceSsao, DestructionRetiresBlockedWorkWithoutFreeingItsHeap)
    {
        const size_t baseline = m_allocator->getAllocationCount();
        bool profilerDetached = false;
        eastl::weak_ptr<d3d::mgpu::ISsaoProfiler> profilerLifetime;
        ComPtr<ID3D12Fence> gate;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        drv3d_dx12::CrossAdapterSyncPoint completion;
        {
            drv3d_dx12::DeviceSsaoPass blocked;
            drv3d_dx12::DeviceSsaoPassDesc desc;
            desc.allocator = m_allocator;
            desc.width = WIDTH;
            desc.height = HEIGHT;
            desc.vertexShader = {m_vertex->GetBufferPointer(), m_vertex->GetBufferSize()};
            desc.rawShader = {m_raw->GetBufferPointer(), m_raw->GetBufferSize()};
            desc.blurShader = {m_blur->GetBufferPointer(), m_blur->GetBufferSize()};
            desc.profiler = eastl::allocate_shared<RetirementProfiler>(eastl::allocator_malloc{}, profilerDetached);
            profilerLifetime = desc.profiler;
            ASSERT_HRESULT_SUCCEEDED(blocked.init(desc));
            fillInputs(false, false, &blocked);
            ASSERT_HRESULT_SUCCEEDED(blocked.execute(m_constants, true, {gate.Get(), 1}));
            completion = blocked.getReadyPoint();
        }
        EXPECT_EQ(drv3d_dx12::DeviceSsaoPass::collectRetiredResources(), 1u);
        EXPECT_TRUE(profilerDetached);
        EXPECT_FALSE(profilerLifetime.expired());
        EXPECT_GT(m_allocator->getAllocationCount(), baseline);
        EXPECT_HRESULT_SUCCEEDED(gate->Signal(1));
        wait(completion);
        EXPECT_EQ(drv3d_dx12::DeviceSsaoPass::collectRetiredResources(), 0u);
        EXPECT_TRUE(profilerLifetime.expired());
        EXPECT_EQ(m_allocator->getAllocationCount(), baseline);
    }
}  // namespace
