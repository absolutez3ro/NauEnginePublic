// test_fullscreen_pass_utils.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// Shared checks for DeviceFullscreenPass tests: shader compilation and pixel verification.
#pragma once

#include <EASTL/unique_ptr.h>
#include <d3dcompiler.h>
#include <gtest/gtest.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>

#include "device_fullscreen_pass.h"

namespace mgpu_test
{
    /**
     * @brief Compiles one entry point of the built-in test pass HLSL to DXBC with the system D3DCompile.
     * @return The bytecode blob, or nullptr after reporting a test failure with the compiler message.
     */
    inline Microsoft::WRL::ComPtr<ID3DBlob> compileTestShader(const char* entryPoint, const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> code;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(drv3d_dx12::FULLSCREEN_TEST_PASS_HLSL, sizeof(drv3d_dx12::FULLSCREEN_TEST_PASS_HLSL) - 1,
                                          "fullscreen_test_pass.hlsl", nullptr, nullptr, entryPoint, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (FAILED(result))
        {
            ADD_FAILURE() << "D3DCompile(" << entryPoint << ") failed: 0x" << std::hex << static_cast<unsigned>(result) << ": "
                          << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
            return {};
        }
        return code;
    }

    /**
     * @brief Expected 8-bit UNORM channel of a float written by the shader (round to nearest).
     */
    inline int toUnorm8(float value)
    {
        return static_cast<int>(value * 255.0f + 0.5f);
    }

    /**
     * @brief Checks every pixel of a tightly packed RGBA8 image against the checkerboard of FULLSCREEN_TEST_PASS_HLSL.
     *
     * Odd (x ^ y) pixels hold the color, even ones its inverse with alpha 1. One unit of tolerance per channel covers
     * different rounding of float to UNORM on different vendors. Only the first mismatch is reported.
     */
    inline void expectCheckerboard(const uint8_t* pixels, UINT width, UINT height, const drv3d_dx12::FullscreenPassColor& color)
    {
        const int odd[4] = {toUnorm8(color.r), toUnorm8(color.g), toUnorm8(color.b), toUnorm8(color.a)};
        const int even[4] = {toUnorm8(1.0f - color.r), toUnorm8(1.0f - color.g), toUnorm8(1.0f - color.b), 255};
        for (UINT y = 0; y < height; ++y)
        {
            for (UINT x = 0; x < width; ++x)
            {
                const int* expected = ((x ^ y) & 1) != 0 ? odd : even;
                const uint8_t* pixel = pixels + (static_cast<size_t>(y) * width + x) * 4;
                for (int channel = 0; channel < 4; ++channel)
                {
                    if (std::abs(static_cast<int>(pixel[channel]) - expected[channel]) > 1)
                    {
                        ADD_FAILURE() << "pixel (" << x << ", " << y << ") channel " << channel << " is " << static_cast<int>(pixel[channel])
                                      << ", expected " << expected[channel];
                        return;
                    }
                }
            }
        }
    }

    /**
     * @brief Checks that a native object was created by the expected device.
     */
    template <typename TObject>
    void expectCreatedBy(TObject* object, ID3D12Device* expected)
    {
        ASSERT_NE(object, nullptr);
        Microsoft::WRL::ComPtr<ID3D12Device> actual;
        ASSERT_HRESULT_SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&actual)));
        EXPECT_EQ(actual.Get(), expected);
    }

    /**
     * @brief Full MG-10 acceptance on one device: create the pass, draw, read back, verify pixels and ownership.
     * @param device The device the pass must run on (GPU 1 in the physical pair test).
     * @param other A different device that must not own any of the pass objects; may be nullptr.
     */
    inline void runPassAndVerify(ID3D12Device* device, ID3D12Device* other, UINT width, UINT height)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader = compileTestShader("vsMain", "vs_5_0");
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader = compileTestShader("psMain", "ps_5_0");
        ASSERT_TRUE(vertexShader && pixelShader);

        drv3d_dx12::DeviceFullscreenPassDesc desc;
        desc.device = device;
        desc.width = width;
        desc.height = height;
        desc.vertexShader = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        desc.pixelShader = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};

        drv3d_dx12::DeviceFullscreenPass pass;
        ASSERT_HRESULT_SUCCEEDED(pass.init(desc));
        EXPECT_EQ(pass.getDevice(), device);
        ASSERT_NO_FATAL_FAILURE(expectCreatedBy(pass.getRenderTarget(), device));
        ASSERT_NO_FATAL_FAILURE(expectCreatedBy(pass.getConstantBuffer(), device));
        ASSERT_NO_FATAL_FAILURE(expectCreatedBy(pass.getPipelineState(), device));
        ASSERT_NO_FATAL_FAILURE(expectCreatedBy(pass.getRootSignature(), device));
        ASSERT_NO_FATAL_FAILURE(expectCreatedBy(pass.getFence(), device));
        if (other)
        {
            // Every object above was created by 'device', so none of them can belong to the other GPU.
            EXPECT_NE(device, other);
        }

        const drv3d_dx12::FullscreenPassColor color = {1.0f, 0.0f, 0.2f, 1.0f};
        pass.setColor(color);
        ASSERT_HRESULT_SUCCEEDED(pass.execute(true));
        EXPECT_EQ(pass.getLastSubmittedValue(), 1u);
        ASSERT_HRESULT_SUCCEEDED(pass.waitForCompletion());
        EXPECT_GE(pass.getFence()->GetCompletedValue(), pass.getLastSubmittedValue());

        eastl::unique_ptr<uint8_t[]> pixels(new uint8_t[static_cast<size_t>(width) * height * 4]);
        ASSERT_HRESULT_SUCCEEDED(pass.readPixels(pixels.get(), static_cast<size_t>(width) * height * 4));
        expectCheckerboard(pixels.get(), width, height, color);
        // pass is destroyed here: it waits for the GPU and releases its objects before the caller releases the device.
    }
}  // namespace mgpu_test
