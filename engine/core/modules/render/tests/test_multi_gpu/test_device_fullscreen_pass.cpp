// Copyright 2026 N-GINN LLC. All rights reserved.

#include <EASTL/unique_ptr.h>
#include <gtest/gtest.h>

#include "device_fullscreen_pass.h"
#include "multi_gpu_device.h"
#include "nau/directx/d3d12sdklayers.h"
#include "test_fullscreen_pass_utils.h"
#include "test_resource_allocator.h"

namespace
{
    using drv3d_dx12::DeviceFullscreenPass;
    using drv3d_dx12::DeviceFullscreenPassDesc;
    using drv3d_dx12::FullscreenPassColor;
    using Microsoft::WRL::ComPtr;

    /**
     * @brief Creates an independent WARP or hardware device for pass tests.
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

    /**
     * @brief Fixture with the D3D12 debug layer enabled; fails the test on any debug ERROR/CORRUPTION message or leak.
     */
    class DeviceFullscreenPassTest : public ::testing::TestWithParam<bool>
    {
    public:
        DeviceFullscreenPassTest() = default;
        DeviceFullscreenPassTest(const DeviceFullscreenPassTest&) = delete;
        DeviceFullscreenPassTest(DeviceFullscreenPassTest&&) = delete;
        DeviceFullscreenPassTest& operator=(const DeviceFullscreenPassTest&) = delete;
        DeviceFullscreenPassTest& operator=(DeviceFullscreenPassTest&&) = delete;
        ~DeviceFullscreenPassTest() override = default;

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
            m_vertexShader = mgpu_test::compileTestShader("vsMain", "vs_5_0");
            m_pixelShader = mgpu_test::compileTestShader("psMain", "ps_5_0");
            ASSERT_TRUE(m_vertexShader && m_pixelShader);
            ASSERT_HRESULT_SUCCEEDED(m_resourceAllocator->init(m_device.Get()));
            if (debug)
            {
                ASSERT_HRESULT_SUCCEEDED(m_device.As(&m_diagnostics));
                m_diagnostics->ClearStoredMessages();
            }
        }

        void TearDown() override
        {
            EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 0u);
            EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 0u);
            EXPECT_HRESULT_SUCCEEDED(m_resourceAllocator->shutdown());
            if (!m_diagnostics)
            {
                return;
            }
            // Passes created by the test have already been destroyed here.
            ComPtr<ID3D12DebugDevice> debugDevice;
            ASSERT_HRESULT_SUCCEEDED(m_device.As(&debugDevice));
            EXPECT_HRESULT_SUCCEEDED(debugDevice->ReportLiveDeviceObjects(static_cast<D3D12_RLDO_FLAGS>(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)));
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

        /**
         * @brief Builds a pass description for the fixture device with the compiled test shaders.
         */
        DeviceFullscreenPassDesc makeDesc(UINT width, UINT height)
        {
            DeviceFullscreenPassDesc desc;
            desc.allocator = m_resourceAllocator;
            desc.width = width;
            desc.height = height;
            desc.vertexShader = {m_vertexShader->GetBufferPointer(), m_vertexShader->GetBufferSize()};
            desc.pixelShader = {m_pixelShader->GetBufferPointer(), m_pixelShader->GetBufferSize()};
            return desc;
        }

    protected:
        ComPtr<ID3D12Device> m_device;
        eastl::shared_ptr<mgpu_test::TestResourceAllocator> m_resourceAllocator = mgpu_test::makeTestResourceAllocator();
        ComPtr<ID3D12InfoQueue> m_diagnostics;
        ComPtr<ID3DBlob> m_vertexShader;
        ComPtr<ID3DBlob> m_pixelShader;
    };

    TEST(DeviceFullscreenPassInput, EmptyPassRejectsUseWithoutFallback)
    {
        DeviceFullscreenPass pass;
        EXPECT_EQ(pass.execute(false), DXGI_ERROR_INVALID_CALL);
        EXPECT_EQ(pass.waitForCompletion(), S_OK);
        EXPECT_EQ(pass.readPixels(nullptr, 16), E_INVALIDARG);
        uint8_t pixel[4] = {};
        EXPECT_EQ(pass.readPixels(pixel, sizeof(pixel)), DXGI_ERROR_INVALID_CALL);
        EXPECT_EQ(pass.getDevice(), nullptr);
        EXPECT_EQ(pass.getRenderTarget(), nullptr);
        EXPECT_EQ(pass.getFence(), nullptr);
        EXPECT_EQ(pass.getLastSubmittedValue(), 0u);
        pass.reset();
        pass.reset();
    }

    TEST(DeviceFullscreenPassInput, NullDeviceDoesNotFallBackToPrimary)
    {
        const uint8_t code[] = {0};
        DeviceFullscreenPassDesc desc;
        desc.width = 16;
        desc.height = 16;
        desc.vertexShader = {code, sizeof(code)};
        desc.pixelShader = {code, sizeof(code)};
        DeviceFullscreenPass pass;
        EXPECT_EQ(pass.init(desc), E_INVALIDARG);
        EXPECT_EQ(pass.getDevice(), nullptr);
    }

    TEST(DeviceFullscreenPassInput, ProcessExitsWithPermanentlyUnsignaledExternalFence)
    {
        // Only WARP is used in the child. It returns from main normally without signaling its consumer fence.
        wchar_t executable[MAX_PATH] = {};
        const DWORD pathSize = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        ASSERT_GT(pathSize, 0u);
        ASSERT_LT(pathSize, static_cast<DWORD>(MAX_PATH));
        wchar_t command[MAX_PATH + 64] = {};
        ASSERT_GT(swprintf_s(command, L"\"%s\" --mgpu-blocked-shutdown-child", executable), 0);
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        ASSERT_TRUE(CreateProcessW(executable, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &startup, &process));
        const DWORD waitResult = WaitForSingleObject(process.hProcess, 10000);
        DWORD exitCode = STILL_ACTIVE;
        EXPECT_TRUE(GetExitCodeProcess(process.hProcess, &exitCode));
        if (waitResult != WAIT_OBJECT_0)
        {
            // A regression must fail the test without leaving the deliberately blocked child running.
            EXPECT_TRUE(TerminateProcess(process.hProcess, 1));
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        EXPECT_EQ(waitResult, WAIT_OBJECT_0);
        if (exitCode == 77)
        {
            GTEST_SKIP() << "D3D12 WARP unavailable in shutdown child";
        }
        EXPECT_EQ(exitCode, 0u);
    }

    TEST(MultiGpuWarpPair, FullscreenPassOnWarpIsIndependentOfPrimaryHardware)
    {
        ComPtr<ID3D12Device> primary = createTestDevice(false);
        ComPtr<ID3D12Device> secondary = createTestDevice(true);
        if (!primary || !secondary)
        {
            GTEST_SKIP() << "Hardware and WARP are required";
        }
        ASSERT_NE(primary.Get(), secondary.Get());
        const LUID primaryLuid = primary->GetAdapterLuid();
        const LUID secondaryLuid = secondary->GetAdapterLuid();
        EXPECT_TRUE(primaryLuid.LowPart != secondaryLuid.LowPart || primaryLuid.HighPart != secondaryLuid.HighPart);
        // WARP is explicitly selected for this acceptance test; production hardware selection stays unchanged.
        ASSERT_NO_FATAL_FAILURE(mgpu_test::runPassAndVerify(secondary.Get(), primary.Get(), 1920, 1080));
    }

    TEST_P(DeviceFullscreenPassTest, RejectsInvalidDescriptionsWithoutAllocating)
    {
        DeviceFullscreenPass pass;
        EXPECT_EQ(pass.init(makeDesc(0, 16)), E_INVALIDARG);
        EXPECT_EQ(pass.init(makeDesc(16, 0)), E_INVALIDARG);
        EXPECT_EQ(pass.init(makeDesc(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1, 16)), E_INVALIDARG);
        DeviceFullscreenPassDesc noVertexShader = makeDesc(16, 16);
        noVertexShader.vertexShader = {};
        EXPECT_EQ(pass.init(noVertexShader), E_INVALIDARG);
        DeviceFullscreenPassDesc noPixelShader = makeDesc(16, 16);
        noPixelShader.pixelShader = {noPixelShader.pixelShader.pShaderBytecode, 0};
        EXPECT_EQ(pass.init(noPixelShader), E_INVALIDARG);
        EXPECT_EQ(pass.getDevice(), nullptr);
        EXPECT_EQ(pass.getRenderTarget(), nullptr);
    }

    TEST_P(DeviceFullscreenPassTest, BrokenShaderBytecodeFailsCleanlyAndLeavesPassEmpty)
    {
        // Valid pointers but not a shader: PSO creation fails after the other objects were created.
        const uint8_t garbage[64] = {1, 2, 3};
        DeviceFullscreenPassDesc desc = makeDesc(16, 16);
        desc.vertexShader = {garbage, sizeof(garbage)};
        DeviceFullscreenPass pass;
        EXPECT_TRUE(FAILED(pass.init(desc)));
        if (m_diagnostics)
        {
            // The debug layer reports the rejected bytecode as an ERROR; that is the expected outcome here.
            m_diagnostics->ClearStoredMessages();
        }
        EXPECT_EQ(pass.getDevice(), nullptr);
        EXPECT_EQ(pass.getRenderTarget(), nullptr);
        EXPECT_EQ(pass.getPipelineState(), nullptr);
        EXPECT_EQ(pass.getFence(), nullptr);
        // The same object can be initialized properly afterwards.
        EXPECT_HRESULT_SUCCEEDED(pass.init(makeDesc(16, 16)));
    }

    TEST_P(DeviceFullscreenPassTest, CreatesAllObjectsOnTheExplicitDevice)
    {
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(64, 32)));
        EXPECT_EQ(pass.getDevice(), m_device.Get());
        ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCreatedBy(pass.getRenderTarget(), m_device.Get()));
        ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCreatedBy(pass.getConstantBuffer(), m_device.Get()));
        ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCreatedBy(pass.getPipelineState(), m_device.Get()));
        ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCreatedBy(pass.getRootSignature(), m_device.Get()));
        ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCreatedBy(pass.getFence(), m_device.Get()));

        const D3D12_RESOURCE_DESC targetDesc = pass.getRenderTarget()->GetDesc();
        EXPECT_EQ(targetDesc.Width, 64u);
        EXPECT_EQ(targetDesc.Height, 32u);
        EXPECT_EQ(targetDesc.Format, DXGI_FORMAT_R8G8B8A8_UNORM);

        // A working pass is never replaced silently.
        ID3D12PipelineState* original = pass.getPipelineState();
        EXPECT_EQ(pass.init(makeDesc(8, 8)), DXGI_ERROR_INVALID_CALL);
        EXPECT_EQ(pass.getPipelineState(), original);
        EXPECT_EQ(pass.getRenderTarget()->GetDesc().Width, 64u);

        pass.reset();
        EXPECT_EQ(pass.getDevice(), nullptr);
        EXPECT_EQ(pass.getPipelineState(), nullptr);
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(8, 8)));
        EXPECT_EQ(pass.getRenderTarget()->GetDesc().Width, 8u);
    }

    TEST_P(DeviceFullscreenPassTest, DrawsCheckerboardAndReadbackMatches)
    {
        // Odd dimensions exercise the row pitch of the readback footprint.
        constexpr UINT WIDTH = 37;
        constexpr UINT HEIGHT = 19;
        ASSERT_NO_FATAL_FAILURE(mgpu_test::runPassAndVerify(m_device.Get(), nullptr, WIDTH, HEIGHT));
    }

    TEST_P(DeviceFullscreenPassTest, NewColorIsUsedByEverySubmissionSlot)
    {
        constexpr UINT WIDTH = 32;
        constexpr UINT HEIGHT = 16;
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(WIDTH, HEIGHT)));
        eastl::unique_ptr<uint8_t[]> pixels(new uint8_t[WIDTH * HEIGHT * 4]);
        // More submissions than DeviceFullscreenPass::FRAME_COUNT, so every constant buffer slot is reused.
        const FullscreenPassColor colors[] = {
            {1.0f, 0.0f, 0.2f, 1.0f},
            {0.0f, 0.0f, 1.0f, 1.0f},
            {0.2f, 1.0f, 0.0f, 1.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            {0.0f, 0.0f, 0.0f, 1.0f}
        };
        for (const FullscreenPassColor& color : colors)
        {
            pass.setColor(color);
            ASSERT_HRESULT_SUCCEEDED(pass.execute(true));
            ASSERT_HRESULT_SUCCEEDED(pass.waitForCompletion());
            ASSERT_HRESULT_SUCCEEDED(pass.readPixels(pixels.get(), WIDTH * HEIGHT * 4));
            ASSERT_NO_FATAL_FAILURE(mgpu_test::expectCheckerboard(pixels.get(), WIDTH, HEIGHT, color));
        }
        EXPECT_EQ(pass.getLastSubmittedValue(), 5u);
    }

    TEST_P(DeviceFullscreenPassTest, QueuedSubmissionsKeepTheirOwnConstants)
    {
        // Submit without waiting in between: with two slots in flight the third submission has to wait on the CPU only
        // for the first one, and no submission may see the color of a later setColor().
        constexpr UINT WIDTH = 16;
        constexpr UINT HEIGHT = 16;
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(WIDTH, HEIGHT)));
        const FullscreenPassColor first = {1.0f, 0.0f, 0.0f, 1.0f};
        const FullscreenPassColor second = {0.0f, 1.0f, 0.0f, 1.0f};
        const FullscreenPassColor last = {0.0f, 0.0f, 1.0f, 1.0f};
        pass.setColor(first);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(false));
        pass.setColor(second);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(false));
        pass.setColor(last);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(true));
        EXPECT_EQ(pass.getLastSubmittedValue(), 3u);
        ASSERT_HRESULT_SUCCEEDED(pass.waitForCompletion());
        EXPECT_EQ(pass.getFence()->GetCompletedValue(), 3u);

        eastl::unique_ptr<uint8_t[]> pixels(new uint8_t[WIDTH * HEIGHT * 4]);
        ASSERT_HRESULT_SUCCEEDED(pass.readPixels(pixels.get(), WIDTH * HEIGHT * 4));
        mgpu_test::expectCheckerboard(pixels.get(), WIDTH, HEIGHT, last);
    }

    TEST_P(DeviceFullscreenPassTest, ReadPixelsRequiresRequestedReadback)
    {
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(8, 8)));
        uint8_t pixels[8 * 8 * 4] = {};
        // Nothing submitted yet.
        EXPECT_EQ(pass.readPixels(pixels, sizeof(pixels)), DXGI_ERROR_INVALID_CALL);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(false));
        ASSERT_HRESULT_SUCCEEDED(pass.waitForCompletion());
        // The submission did not copy into the readback buffer.
        EXPECT_EQ(pass.readPixels(pixels, sizeof(pixels)), DXGI_ERROR_INVALID_CALL);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(true));
        ASSERT_HRESULT_SUCCEEDED(pass.waitForCompletion());
        EXPECT_EQ(pass.readPixels(nullptr, sizeof(pixels)), E_INVALIDARG);
        EXPECT_EQ(pass.readPixels(pixels, sizeof(pixels) - 1), E_INVALIDARG);
        EXPECT_HRESULT_SUCCEEDED(pass.readPixels(pixels, sizeof(pixels)));
    }

    TEST_P(DeviceFullscreenPassTest, ResetAfterSubmissionWaitsForTheGpu)
    {
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(256, 256)));
        for (int index = 0; index < 4; ++index)
        {
            ASSERT_HRESULT_SUCCEEDED(pass.execute(true));
        }
        // No explicit wait: reset() must finish the queued work before releasing anything.
        // The fixture's debug layer reports a destroyed-while-in-use object as an ERROR.
        pass.reset();
        EXPECT_EQ(pass.getFence(), nullptr);
        EXPECT_EQ(pass.getLastSubmittedValue(), 0u);
    }

    TEST_P(DeviceFullscreenPassTest, TimeoutKeepsResourcesUntilExternalWorkCompletes)
    {
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(16, 16)));
        ComPtr<ID3D12Fence> consumer;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer)));
        ASSERT_HRESULT_SUCCEEDED(pass.waitForExternalWork(consumer.Get(), 1));
        // Avoid fatal assertions until the artificial queue dependency has been released.
        EXPECT_HRESULT_SUCCEEDED(pass.execute(true));
        ID3D12Resource* target = pass.getRenderTarget();
        EXPECT_EQ(pass.reset(0), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        EXPECT_EQ(pass.getRenderTarget(), target);
        EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 3u);
        EXPECT_EQ(pass.readPixels(nullptr, 0), E_INVALIDARG);
        EXPECT_HRESULT_SUCCEEDED(consumer->Signal(1));
        EXPECT_HRESULT_SUCCEEDED(pass.waitForCompletion());
        uint8_t pixels[16 * 16 * 4] = {};
        EXPECT_HRESULT_SUCCEEDED(pass.readPixels(pixels, sizeof(pixels)));
        mgpu_test::expectCheckerboard(pixels, 16, 16, FullscreenPassColor{});
        EXPECT_HRESULT_SUCCEEDED(pass.reset());
        EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 0u);
    }

    TEST_P(DeviceFullscreenPassTest, RejectsForeignConsumerFence)
    {
        ComPtr<ID3D12Device> other = createTestDevice(!GetParam());
        if (!other)
        {
            GTEST_SKIP() << "Hardware and WARP are required for the independent-device check";
        }
        ComPtr<ID3D12Fence> foreign;
        ASSERT_HRESULT_SUCCEEDED(other->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreign)));
        DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(makeDesc(16, 16)));
        EXPECT_EQ(pass.waitForExternalWork(nullptr, 1), E_INVALIDARG);
        EXPECT_EQ(pass.waitForExternalWork(foreign.Get(), 1), E_INVALIDARG);
        EXPECT_EQ(pass.getLastSubmittedValue(), 0u);
    }

    TEST_P(DeviceFullscreenPassTest, DestructorRetiresBlockedWorkAndKeepsAllocatorAlive)
    {
        eastl::shared_ptr<mgpu_test::TestResourceAllocator> allocator = mgpu_test::makeTestResourceAllocator();
        ASSERT_HRESULT_SUCCEEDED(allocator->init(m_device.Get()));
        eastl::weak_ptr<mgpu_test::TestResourceAllocator> lifetime = allocator;
        ComPtr<ID3D12Fence> consumer;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer)));

        DeviceFullscreenPassDesc desc = makeDesc(16, 16);
        desc.allocator = allocator;
        eastl::unique_ptr<DeviceFullscreenPass> pass(new DeviceFullscreenPass());
        ASSERT_HRESULT_SUCCEEDED(pass->init(desc));
        ASSERT_HRESULT_SUCCEEDED(pass->waitForExternalWork(consumer.Get(), 1));
        EXPECT_HRESULT_SUCCEEDED(pass->execute(true));
        ComPtr<ID3D12Fence> completion = pass->getFence();
        const UINT64 value = pass->getLastSubmittedValue();
        // The original allocator owner disappears before the blocked pass is destroyed.
        desc.allocator.reset();
        allocator.reset();
        const ULONGLONG started = GetTickCount64();
        pass.reset();
        EXPECT_LT(GetTickCount64() - started, 1000u);
        EXPECT_FALSE(lifetime.expired());
        EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 1u);
        EXPECT_LT(completion->GetCompletedValue(), value);
        {
            eastl::shared_ptr<mgpu_test::TestResourceAllocator> retained = lifetime.lock();
            EXPECT_TRUE(retained);
            if (retained)
            {
                EXPECT_EQ(retained->getAllocationCount(), 3u);
                EXPECT_EQ(retained->shutdown(), DXGI_ERROR_INVALID_CALL);
            }
        }

        // Only the consumer releases the dependency. Retirement must not fake this signal.
        EXPECT_HRESULT_SUCCEEDED(consumer->Signal(1));
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (DeviceFullscreenPass::collectRetiredResources() != 0 && GetTickCount64() < deadline)
        {
            Sleep(1);
        }
        EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 0u);
        EXPECT_GE(completion->GetCompletedValue(), value);
        EXPECT_TRUE(lifetime.expired());
        // Fixture diagnostics also reject destroyed-while-in-use objects and resource/heap leaks.
    }

    TEST_P(DeviceFullscreenPassTest, RetirementCollectsCompletedWorkBehindBlockedEntry)
    {
        ComPtr<ID3D12Fence> readyConsumer;
        ComPtr<ID3D12Fence> blockedConsumer;
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readyConsumer)));
        ASSERT_HRESULT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blockedConsumer)));
        eastl::unique_ptr<DeviceFullscreenPass> ready(new DeviceFullscreenPass());
        eastl::unique_ptr<DeviceFullscreenPass> blocked(new DeviceFullscreenPass());
        ASSERT_HRESULT_SUCCEEDED(ready->init(makeDesc(16, 16)));
        ASSERT_HRESULT_SUCCEEDED(blocked->init(makeDesc(16, 16)));
        ASSERT_HRESULT_SUCCEEDED(ready->waitForExternalWork(readyConsumer.Get(), 1));
        EXPECT_HRESULT_SUCCEEDED(ready->execute(false));
        EXPECT_HRESULT_SUCCEEDED(blocked->waitForExternalWork(blockedConsumer.Get(), 1));
        EXPECT_HRESULT_SUCCEEDED(blocked->execute(false));
        ComPtr<ID3D12Fence> readyCompletion = ready->getFence();
        const UINT64 readyValue = ready->getLastSubmittedValue();
        ready.reset();
        blocked.reset();  // The blocked entry is at the head of the intrusive retirement list.
        EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 2u);
        EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 6u);

        EXPECT_HRESULT_SUCCEEDED(readyConsumer->Signal(1));
        ULONGLONG deadline = GetTickCount64() + 5000;
        while (readyCompletion->GetCompletedValue() < readyValue && GetTickCount64() < deadline)
        {
            Sleep(1);
        }
        EXPECT_GE(readyCompletion->GetCompletedValue(), readyValue);
        EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 1u);
        EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 3u);
        EXPECT_EQ(blockedConsumer->GetCompletedValue(), 0u);

        EXPECT_HRESULT_SUCCEEDED(blockedConsumer->Signal(1));
        deadline = GetTickCount64() + 5000;
        while (DeviceFullscreenPass::collectRetiredResources() != 0 && GetTickCount64() < deadline)
        {
            Sleep(1);
        }
        EXPECT_EQ(DeviceFullscreenPass::collectRetiredResources(), 0u);
        EXPECT_EQ(m_resourceAllocator->getAllocationCount(), 0u);
    }

    INSTANTIATE_TEST_SUITE_P(NativeDevices, DeviceFullscreenPassTest, ::testing::Values(true, false), [](const ::testing::TestParamInfo<bool>& info)
    {
        return info.param ? "Warp" : "Hardware";
    });

}  // namespace

namespace mgpu_test
{
    int runBlockedShutdownChild()
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device = createTestDevice(true);
        if (!device)
        {
            return 77;
        }
        {
            eastl::shared_ptr<TestResourceAllocator> allocator = makeTestResourceAllocator();
            Microsoft::WRL::ComPtr<ID3DBlob> vertex = compileTestShader("vsMain", "vs_5_0");
            Microsoft::WRL::ComPtr<ID3DBlob> pixel = compileTestShader("psMain", "ps_5_0");
            if (!vertex || !pixel || FAILED(allocator->init(device.Get())))
            {
                return 1;
            }
            drv3d_dx12::DeviceFullscreenPassDesc desc;
            desc.allocator = allocator;
            desc.width = 16;
            desc.height = 16;
            desc.vertexShader = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
            desc.pixelShader = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
            Microsoft::WRL::ComPtr<ID3D12Fence> consumer;
            drv3d_dx12::DeviceFullscreenPass pass;
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer))) ||
                FAILED(pass.init(desc)) || FAILED(pass.waitForExternalWork(consumer.Get(), 1)) ||
                FAILED(pass.execute(false)))
            {
                return 2;
            }
            // Neither this scope nor process shutdown is allowed to wait for consumer->Signal(1).
        }
        return drv3d_dx12::DeviceFullscreenPass::collectRetiredResources() == 1 ? 0 : 3;
    }
}  // namespace mgpu_test
