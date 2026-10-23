// test_ssao_shader.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
// Execute the production HLSL on WARP. D3D11 is only a compact shader test harness;
// the SceneBaseSample runtime test separately exercises Nau's D3D12 frame graph.
#include <EASTL/allocator_malloc.h>
#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <gtest/gtest.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    using Microsoft::WRL::ComPtr;
    template <typename TValue>
    using TestVector = eastl::vector<TValue, eastl::allocator_malloc>;

    constexpr UINT WIDTH = 79;
    constexpr UINT HEIGHT = 53;
    constexpr UINT PIXELS = WIDTH * HEIGHT;
    constexpr float NEAR_PLANE = 0.1f;
    constexpr float FAR_PLANE = 100.0f;
    constexpr float PROJECTION_A = NEAR_PLANE / (FAR_PLANE - NEAR_PLANE);
    constexpr float PROJECTION_B = NEAR_PLANE * FAR_PLANE / (FAR_PLANE - NEAR_PLANE);

    struct Constants
    {
        float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, PROJECTION_A, -1, 0, 0, PROJECTION_B, 0};
        float inverseProjection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 / PROJECTION_B, 0, 0, -1, PROJECTION_A / PROJECTION_B};
        float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float parameters[4] = {0.5f, 0.2f, 1.0f, 0.05f};
        float filter[4] = {1, 0, 0.2f, 1};
    };

    float projectDepth(float viewZ)
    {
        return (PROJECTION_A * viewZ + PROJECTION_B) / -viewZ;
    }

    class ShaderIncludes final : public ID3DInclude
    {
    public:
        HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
        {
            if (std::strcmp(name, "ssao_common.hlsli") != 0)
            {
                return E_FAIL;
            }
            FILE* file = nullptr;
            if (fopen_s(&file, NAU_SSAO_SHADER_DIR "/include/ssao_common.hlsli", "rb") != 0)
            {
                return E_FAIL;
            }
            fseek(file, 0, SEEK_END);
            m_text.resize(static_cast<size_t>(ftell(file)));
            rewind(file);
            const size_t read = fread(m_text.data(), 1, m_text.size(), file);
            fclose(file);
            *data = m_text.data();
            *bytes = static_cast<UINT>(read);
            return read == m_text.size() ? S_OK : E_FAIL;
        }
        HRESULT Close(LPCVOID) override
        {
            return S_OK;
        }

    private:
        TestVector<char> m_text;
    };

    class SsaoShader : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                                       nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context));
            ASSERT_HRESULT_SUCCEEDED(m_device.As(&m_debug));
            m_debug->ClearStoredMessages();
            m_normals.resize(PIXELS * 4, 0.0f);
            m_depth.resize(PIXELS, projectDepth(-3.0f));
            m_input.resize(PIXELS, 1.0f);
            for (UINT pixel = 0; pixel < PIXELS; ++pixel)
            {
                m_normals[pixel * 4] = 0.5f;
                m_normals[pixel * 4 + 1] = 0.5f;
            }
        }

        void TearDown() override
        {
            if (!m_debug)
            {
                return;
            }
            for (UINT64 index = 0; index < m_debug->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0;
                ASSERT_HRESULT_SUCCEEDED(m_debug->GetMessage(index, nullptr, &bytes));
                TestVector<char> storage(bytes);
                D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                ASSERT_HRESULT_SUCCEEDED(m_debug->GetMessage(index, message, &bytes));
                EXPECT_GT(message->Severity, D3D11_MESSAGE_SEVERITY_WARNING) << message->pDescription;
            }
        }

        ComPtr<ID3DBlob> compile(bool blur, bool vertex)
        {
            const wchar_t* path = blur ? L"ssao_blur.hlsl" : L"ssao.hlsl";
            wchar_t fullPath[1024] = {};
            swprintf_s(fullPath, L"%hs/src/%ls", NAU_SSAO_SHADER_DIR, path);
            ShaderIncludes includes;
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> errors;
            const HRESULT result = D3DCompileFromFile(fullPath, nullptr, &includes, vertex ? "VSMain" : "PSMain",
                                                      vertex ? "vs_5_0" : "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                                                      0, &blob, &errors);
            EXPECT_HRESULT_SUCCEEDED(result) << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "No compiler diagnostics");
            return blob;
        }

        TestVector<float> render(bool blur = false)
        {
            ComPtr<ID3DBlob> vertex = compile(blur, true);
            ComPtr<ID3DBlob> pixel = compile(blur, false);
            if (!vertex || !pixel)
            {
                return TestVector<float>(PIXELS, -1.0f);
            }
            ComPtr<ID3D11VertexShader> vertexShader;
            ComPtr<ID3D11PixelShader> pixelShader;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertexShader));
            EXPECT_HRESULT_SUCCEEDED(m_device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &pixelShader));
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = sizeof(Constants);
            bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
            bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA constantsData = {&m_constants, 0, 0};
            ComPtr<ID3D11Buffer> constants;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateBuffer(&bufferDesc, &constantsData, &constants));
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = WIDTH;
            textureDesc.Height = HEIGHT;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            eastl::array<ComPtr<ID3D11Texture2D>, 3> textures;
            eastl::array<ComPtr<ID3D11ShaderResourceView>, 3> views;
            const float* inputs[] = {m_normals.data(), m_depth.data(), m_input.data()};
            for (UINT index = 0; index < 3; ++index)
            {
                textureDesc.Format = index == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
                D3D11_SUBRESOURCE_DATA data = {inputs[index], WIDTH * sizeof(float) * (index == 0 ? 4u : 1u), 0};
                EXPECT_HRESULT_SUCCEEDED(m_device->CreateTexture2D(&textureDesc, &data, &textures[index]));
                EXPECT_HRESULT_SUCCEEDED(m_device->CreateShaderResourceView(textures[index].Get(), nullptr, &views[index]));
            }
            textureDesc.Format = DXGI_FORMAT_R32_FLOAT;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> output;
            ComPtr<ID3D11RenderTargetView> target;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateTexture2D(&textureDesc, nullptr, &output));
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateRenderTargetView(output.Get(), nullptr, &target));
            textureDesc.Usage = D3D11_USAGE_STAGING;
            textureDesc.BindFlags = 0;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateTexture2D(&textureDesc, nullptr, &readback));
            D3D11_RASTERIZER_DESC rasterDesc = {};
            rasterDesc.FillMode = D3D11_FILL_SOLID;
            rasterDesc.CullMode = D3D11_CULL_NONE;
            rasterDesc.DepthClipEnable = TRUE;
            ComPtr<ID3D11RasterizerState> raster;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateRasterizerState(&rasterDesc, &raster));
            D3D11_DEPTH_STENCIL_DESC depthDesc = {};
            ComPtr<ID3D11DepthStencilState> depthState;
            EXPECT_HRESULT_SUCCEEDED(m_device->CreateDepthStencilState(&depthDesc, &depthState));
            const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(WIDTH), static_cast<float>(HEIGHT), 0, 1};
            ID3D11ShaderResourceView* resources[] = {views[0].Get(), views[1].Get(), views[2].Get()};
            m_context->RSSetViewports(1, &viewport);
            m_context->RSSetState(raster.Get());
            m_context->OMSetDepthStencilState(depthState.Get(), 0);
            m_context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(vertexShader.Get(), nullptr, 0);
            m_context->PSSetShader(pixelShader.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            m_context->PSSetShaderResources(0, 3, resources);
            m_context->Draw(3, 0);
            m_context->CopyResource(readback.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            TestVector<float> result(PIXELS, -1.0f);
            const HRESULT mappedResult = m_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            EXPECT_HRESULT_SUCCEEDED(mappedResult);
            if (SUCCEEDED(mappedResult))
            {
                for (UINT row = 0; row < HEIGHT; ++row)
                {
                    std::memcpy(result.data() + row * WIDTH, static_cast<const char*>(mapped.pData) + row * mapped.RowPitch, WIDTH * sizeof(float));
                }
                m_context->Unmap(readback.Get(), 0);
            }
            m_context->ClearState();
            return result;
        }

        void expectConstant(const TestVector<float>& pixels, float value)
        {
            for (float pixel : pixels)
            {
                ASSERT_TRUE(std::isfinite(pixel));
                ASSERT_NEAR(pixel, value, 0.0001f);
            }
        }

        void makeDepthStep(float nearZ)
        {
            for (UINT y = 0; y < HEIGHT; ++y)
            {
                for (UINT x = 0; x < WIDTH / 2; ++x)
                {
                    m_depth[y * WIDTH + x] = projectDepth(nearZ);
                }
            }
        }

        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11InfoQueue> m_debug;
        Constants m_constants;
        TestVector<float> m_normals;
        TestVector<float> m_depth;
        TestVector<float> m_input;
    };

    TEST_F(SsaoShader, EmptyDepthIsUnoccluded)
    {
        eastl::fill(m_depth.begin(), m_depth.end(), 0.0f);
        expectConstant(render(), 1.0f);
        expectConstant(render(true), 1.0f);
    }

    TEST_F(SsaoShader, FlatSurfaceHasNoSelfOcclusion)
    {
        expectConstant(render(), 1.0f);
    }

    TEST_F(SsaoShader, TiltedSurfaceHasNoSelfOcclusion)
    {
        // Plane -0.6*x + 0.8*z + 2.4 = 0, unit normal facing the eye.
        const float encodedX = 0.5f - std::sqrt(0.1f) * 0.5f;
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH; ++x)
            {
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / WIDTH - 1.0f;
                const float viewZ = -2.4f / (0.6f * ndcX + 0.8f);
                m_depth[y * WIDTH + x] = projectDepth(viewZ);
                m_normals[(y * WIDTH + x) * 4] = encodedX;
            }
        }
        expectConstant(render(), 1.0f);
    }

    TEST_F(SsaoShader, NearbyOccluderDarkensOnlyContactRegion)
    {
        makeDepthStep(-2.75f);
        const TestVector<float> pixels = render();
        UINT darkened = 0;
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH; ++x)
            {
                const float pixel = pixels[y * WIDTH + x];
                ASSERT_TRUE(std::isfinite(pixel));
                ASSERT_GE(pixel, 0.0f);
                ASSERT_LE(pixel, 1.0f);
                darkened += pixel < 0.95f ? 1 : 0;
                if (x < WIDTH / 2 || x > WIDTH / 2 + 10)
                {
                    ASSERT_NEAR(pixel, 1.0f, 0.0001f);
                }
            }
        }
        EXPECT_GT(darkened, 20u);
        EXPECT_EQ(pixels, render());
    }

    TEST_F(SsaoShader, ZeroRadiusAndDisabledModeAreNeutral)
    {
        makeDepthStep(-2.75f);
        m_constants.parameters[0] = 0.0f;
        expectConstant(render(), 1.0f);
        m_constants.parameters[0] = 0.5f;
        m_constants.filter[3] = 0.0f;
        expectConstant(render(), 1.0f);
    }

    TEST_F(SsaoShader, WorldNormalsAreRotatedIntoCameraSpace)
    {
        makeDepthStep(-2.75f);
        const TestVector<float> reference = render();
        const float rotation[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1};
        std::memcpy(m_constants.view, rotation, sizeof(rotation));
        for (UINT index = 0; index < PIXELS; ++index)
        {
            m_normals[index * 4] = 0.5f + std::sqrt(0.5f) * 0.5f;
        }
        const TestVector<float> rotated = render();
        for (UINT index = 0; index < PIXELS; ++index)
        {
            ASSERT_NEAR(reference[index], rotated[index], 0.0001f);
        }
    }

    TEST_F(SsaoShader, FarOccluderDoesNotCastAmbientShadow)
    {
        makeDepthStep(-1.0f);
        expectConstant(render(), 1.0f);
    }

    TEST_F(SsaoShader, BackgroundCannotOccludeTheSilhouette)
    {
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH / 2; ++x)
            {
                m_depth[y * WIDTH + x] = 0.0f;
            }
        }
        expectConstant(render(), 1.0f);
    }

    TEST_F(SsaoShader, BlurPreservesConstantAndDepthDiscontinuity)
    {
        eastl::fill(m_input.begin(), m_input.end(), 0.3f);
        expectConstant(render(true), 0.3f);
        makeDepthStep(-2.5f);
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH; ++x)
            {
                m_input[y * WIDTH + x] = x < WIDTH / 2 ? 0.2f : 0.8f;
            }
        }
        const TestVector<float> result = render(true);
        for (UINT index = 0; index < PIXELS; ++index)
        {
            ASSERT_NEAR(result[index], m_input[index], 0.0001f);
        }
    }

    TEST_F(SsaoShader, BlurPreservesNormalDiscontinuity)
    {
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH; ++x)
            {
                m_input[y * WIDTH + x] = x < WIDTH / 2 ? 0.2f : 0.8f;
                if (x < WIDTH / 2)
                {
                    m_normals[(y * WIDTH + x) * 4] = 0.5f + std::sqrt(0.5f) * 0.5f;
                }
            }
        }
        const TestVector<float> result = render(true);
        for (UINT index = 0; index < PIXELS; ++index)
        {
            ASSERT_NEAR(result[index], m_input[index], 0.0001f);
        }
    }

    TEST_F(SsaoShader, BothBlurDirectionsSmoothNoise)
    {
        for (UINT y = 0; y < HEIGHT; ++y)
        {
            for (UINT x = 0; x < WIDTH; ++x)
            {
                m_input[y * WIDTH + x] = (x + y) % 2 ? 0.0f : 1.0f;
            }
        }
        m_input = render(true);
        m_constants.filter[0] = 0;
        m_constants.filter[1] = 1;
        const TestVector<float> result = render(true);
        for (UINT y = 6; y < HEIGHT - 6; ++y)
        {
            for (UINT x = 6; x < WIDTH - 6; ++x)
            {
                ASSERT_NEAR(result[y * WIDTH + x], 0.5f, 0.01f);
            }
        }
    }
}  // namespace
