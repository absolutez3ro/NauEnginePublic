// Copyright 2026 N-GINN LLC. All rights reserved.

#include <EASTL/unique_ptr.h>
#include <gtest/gtest.h>

#include "device_fullscreen_pass.h"
#include "multi_gpu_device.h"
#include "nau/directx/d3d12sdklayers.h"
#include "test_fullscreen_pass_utils.h"

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
        DeviceFullscreenPassDesc makeDesc(UINT width, UINT height) const
        {
            DeviceFullscreenPassDesc desc;
            desc.device = m_device.Get();
            desc.width = width;
            desc.height = height;
            desc.vertexShader = {m_vertexShader->GetBufferPointer(), m_vertexShader->GetBufferSize()};
            desc.pixelShader = {m_pixelShader->GetBufferPointer(), m_pixelShader->GetBufferSize()};
            return desc;
        }

    protected:
        ComPtr<ID3D12Device> m_device;
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

    INSTANTIATE_TEST_SUITE_P(NativeDevices, DeviceFullscreenPassTest, ::testing::Values(true, false), [](const ::testing::TestParamInfo<bool>& info)
    {
        return info.param ? "Warp" : "Hardware";
    });

}  // namespace
