#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/RenderModeFactory.h"
#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Utils/d3dx12.h"
#include "nau/mgpu/Graphics/GRootSignature.h"

#include <filesystem>
#include <initializer_list>
#include <system_error>

using namespace nau::PEPEngine;
using namespace Graphics;
using namespace Utils;

std::unordered_map<std::string, std::shared_ptr<GShader>> RenderModeFactory::shaders;

namespace
{
    bool ShaderFileExists(const std::wstring& fileName)
    {
        std::error_code error;
        return std::filesystem::exists(std::filesystem::path(fileName), error);
    }

    void LogMissingShaderFile(const std::wstring& fileName)
    {
        const auto message = L"[RenderModeFactory] Shader file not found: " + fileName + L"\n";
        OutputDebugStringW(message.c_str());
    }

    void LogMissingPsoShader(const std::string& shaderName, const RenderMode renderMode)
    {
        const auto message = "[RenderModeFactory] Shader \"" + shaderName +
            "\" is not loaded. Skip PSO " + std::to_string(static_cast<int>(renderMode)) + ".\n";
        OutputDebugStringA(message.c_str());
    }
}

void RenderModeFactory::LoadDefaultPSO(std::shared_ptr<GDevice> device, std::shared_ptr<GRootSignature> rootSignature,
                                       D3D12_INPUT_LAYOUT_DESC defautlInputDesc, DXGI_FORMAT backBufferFormat,
                                       DXGI_FORMAT depthStencilFormat,
                                       std::shared_ptr<GRootSignature> ssaoRootSignature, DXGI_FORMAT normalMapFormat,
                                       DXGI_FORMAT ambientMapFormat, const bool includeSsr)
{
    this->PSO.clear();

    const auto findShader = [&](const std::string& shaderName, const RenderMode renderMode) -> const GShader*
    {
        const auto shader = shaders.find(shaderName);
        if (shader == shaders.end() || shader->second == nullptr)
        {
            LogMissingPsoShader(shaderName, renderMode);
            return nullptr;
        }

        return shader->second.get();
    };

    const auto setShaders = [&](GraphicPSO& pipelineState, const std::initializer_list<const char*> shaderNames)
    {
        bool result = true;
        for (const auto shaderName : shaderNames)
        {
            const auto shader = findShader(shaderName, pipelineState.GetRenderMode());
            if (shader == nullptr)
            {
                result = false;
                continue;
            }

            pipelineState.SetShader(shader);
        }

        return result;
    };

    const auto addPso = [&](std::shared_ptr<GraphicPSO>& pipelineState, const bool canCompile)
    {
        if (!canCompile)
        {
            return;
        }

        const auto renderMode = pipelineState->GetRenderMode();
        this->PSO[renderMode] = std::move(pipelineState);
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;

    ZeroMemory(&basePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    basePsoDesc.InputLayout = defautlInputDesc;
    basePsoDesc.pRootSignature = rootSignature->GetNativeSignature().Get();
    basePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    basePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    basePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    basePsoDesc.SampleMask = UINT_MAX;
    basePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    basePsoDesc.NumRenderTargets = 1;
    basePsoDesc.RTVFormats[0] = GetSRGBFormat(backBufferFormat);
    basePsoDesc.SampleDesc.Count = 1;
    basePsoDesc.SampleDesc.Quality = 0;
    basePsoDesc.DSVFormat = depthStencilFormat;

    auto depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    auto rasterizedDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);


    auto opaquePSO = std::make_shared<GraphicPSO>(RenderMode::Opaque);
    opaquePSO->SetPsoDesc(basePsoDesc);
    const auto opaquePsoShadersLoaded = setShaders(*opaquePSO, {"StandardVertex", "OpaquePixel"});
    depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    opaquePSO->SetDepthStencilState(depthStencilDesc);

    auto alphaDropPso = std::make_shared<GraphicPSO>(RenderMode::OpaqueAlphaDrop);
    alphaDropPso->SetPsoDesc(opaquePSO->GetPsoDescription());
    const auto alphaDropPsoShadersLoaded = setShaders(*alphaDropPso, {"StandardVertex", "AlphaDrop"});
    rasterizedDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizedDesc.CullMode = D3D12_CULL_MODE_NONE;
    alphaDropPso->SetRasterizationState(rasterizedDesc);


    auto shadowMapPSO = std::make_shared<GraphicPSO>(RenderMode::ShadowMapOpaque);
    basePsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowMapPSO->SetPsoDesc(basePsoDesc);
    const auto shadowMapPsoShadersLoaded = setShaders(*shadowMapPSO, {"shadowVS", "shadowOpaquePS"});
    shadowMapPSO->SetRTVFormat(0, DXGI_FORMAT_UNKNOWN);
    shadowMapPSO->SetRenderTargetsCount(0);

    rasterizedDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizedDesc.DepthBias = 100000;
    rasterizedDesc.DepthBiasClamp = 0.0f;
    rasterizedDesc.SlopeScaledDepthBias = 1.0f;
    shadowMapPSO->SetRasterizationState(rasterizedDesc);


    auto shadowMapDropPSO = std::make_shared<GraphicPSO>(RenderMode::ShadowMapOpaqueDrop);
    shadowMapDropPSO->SetPsoDesc(shadowMapPSO->GetPsoDescription());
    const auto shadowMapDropPsoShadersLoaded = setShaders(*shadowMapDropPSO, {"shadowVS", "shadowOpaqueDropPS"});


    auto drawNormalsPso = std::make_shared<GraphicPSO>(RenderMode::DrawNormalsOpaque);
    basePsoDesc.DSVFormat = depthStencilFormat;
    drawNormalsPso->SetPsoDesc(basePsoDesc);
    const auto drawNormalsPsoShadersLoaded = setShaders(*drawNormalsPso, {"drawNormalsVS", "drawNormalsPS"});
    drawNormalsPso->SetRTVFormat(0, normalMapFormat);
    drawNormalsPso->SetSampleCount(1);
    drawNormalsPso->SetSampleQuality(0);
    drawNormalsPso->SetDSVFormat(depthStencilFormat);

    auto drawNormalsDropPso = std::make_shared<GraphicPSO>(*rootSignature.get(), RenderMode::DrawNormalsOpaqueDrop);
    drawNormalsDropPso->SetPsoDesc(drawNormalsPso->GetPsoDescription());
    const auto drawNormalsDropPsoShadersLoaded = setShaders(*drawNormalsDropPso, {"drawNormalsVS", "drawNormalsAlphaDropPS"});
    rasterizedDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizedDesc.CullMode = D3D12_CULL_MODE_NONE;
    drawNormalsDropPso->SetRasterizationState(rasterizedDesc);

    auto skyBoxPSO = std::make_shared<GraphicPSO>(RenderMode::SkyBox);
    skyBoxPSO->SetPsoDesc(basePsoDesc);
    const auto skyBoxPsoShadersLoaded = setShaders(*skyBoxPSO, {"SkyBoxVertex", "SkyBoxPixel"});

    depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyBoxPSO->SetDepthStencilState(depthStencilDesc);
    rasterizedDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizedDesc.CullMode = D3D12_CULL_MODE_NONE;
    skyBoxPSO->SetRasterizationState(rasterizedDesc);


    auto transperentPSO = std::make_shared<GraphicPSO>(RenderMode::Transparent);
    transperentPSO->SetPsoDesc(basePsoDesc);
    const auto transparentPsoShadersLoaded = setShaders(*transperentPSO, {"StandardVertex", "OpaquePixel"});
    D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc = {};
    transparencyBlendDesc.BlendEnable = true;
    transparencyBlendDesc.LogicOpEnable = false;
    transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    transperentPSO->SetRenderTargetBlendState(0, transparencyBlendDesc);

    if (ssaoRootSignature != nullptr)
    {
        auto ssaoPSO = std::make_shared<GraphicPSO>(RenderMode::Ssao);
        ssaoPSO->SetPsoDesc(basePsoDesc);
        ssaoPSO->SetRootSignature(*ssaoRootSignature.get());
        ssaoPSO->SetInputLayout({nullptr, 0});
        const auto ssaoPsoShadersLoaded = setShaders(*ssaoPSO, {"ssaoVS", "ssaoPS"});
        ssaoPSO->SetRTVFormat(0, ambientMapFormat);
        ssaoPSO->SetSampleCount(1);
        ssaoPSO->SetSampleQuality(0);
        ssaoPSO->SetDSVFormat(DXGI_FORMAT_UNKNOWN);
        depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        depthStencilDesc.DepthEnable = false;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ssaoPSO->SetDepthStencilState(depthStencilDesc);


        auto ssaoBlurPSO = std::make_shared<GraphicPSO>(RenderMode::SsaoBlur);
        ssaoBlurPSO->SetPsoDesc(ssaoPSO->GetPsoDescription());
        const auto ssaoBlurPsoShadersLoaded = setShaders(*ssaoBlurPSO, {"ssaoBlurVS", "ssaoBlurPS"});


        addPso(ssaoPSO, ssaoPsoShadersLoaded);
        addPso(ssaoBlurPSO, ssaoBlurPsoShadersLoaded);
    }

    addPso(opaquePSO, opaquePsoShadersLoaded);
    addPso(transperentPSO, transparentPsoShadersLoaded);
    addPso(alphaDropPso, alphaDropPsoShadersLoaded);
    addPso(skyBoxPSO, skyBoxPsoShadersLoaded);
    addPso(shadowMapPSO, shadowMapPsoShadersLoaded);
    addPso(shadowMapDropPSO, shadowMapDropPsoShadersLoaded);
    addPso(drawNormalsPso, drawNormalsPsoShadersLoaded);
    addPso(drawNormalsDropPso, drawNormalsDropPsoShadersLoaded);

    for (auto& pipelineState : PSO)
    {
        pipelineState.second->Initialize(device);
    }
}

void RenderModeFactory::LoadDefaultShaders(const bool includeSsr)
{
    constexpr D3D_SHADER_MACRO defines[] =
    {
        "FOG", "1",
        nullptr, nullptr
    };

    constexpr D3D_SHADER_MACRO alphaTestDefines[] =
    {
        "FOG", "1",
        "ALPHA_TEST", "1",
        nullptr, nullptr
    };

    const auto loadShader = [](const std::string& name, const std::wstring& fileName, const ShaderType type,
                               const D3D_SHADER_MACRO* shaderDefines, const std::string& entryPoint,
                               const std::string& target)
    {
        if (!ShaderFileExists(fileName))
        {
            LogMissingShaderFile(fileName);
            return;
        }

        auto shader = std::make_shared<GShader>(fileName, type, shaderDefines, entryPoint, target);
        shader->LoadAndCompile();
        shaders[name] = std::move(shader);
    };

    if (shaders.empty())
    {
        loadShader("StandardVertex", L"Shaders\\Default.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
        loadShader("AlphaDrop", L"Shaders\\Default.hlsl", PixelShader, alphaTestDefines, "PS", "ps_5_1");
        loadShader("shadowVS", L"Shaders\\Shadows.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
        loadShader("shadowOpaquePS", L"Shaders\\Shadows.hlsl", PixelShader, nullptr, "PS", "ps_5_1");
        loadShader("shadowOpaqueDropPS", L"Shaders\\Shadows.hlsl", PixelShader, alphaTestDefines, "PS", "ps_5_1");
        loadShader("OpaquePixel", L"Shaders\\Default.hlsl", PixelShader, defines, "PS", "ps_5_1");
        loadShader("SkyBoxVertex", L"Shaders\\SkyBoxShader.hlsl", VertexShader, defines, "SKYMAP_VS", "vs_5_1");
        loadShader("SkyBoxPixel", L"Shaders\\SkyBoxShader.hlsl", PixelShader, defines, "SKYMAP_PS", "ps_5_1");

        loadShader("drawNormalsVS", L"Shaders\\DrawNormals.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
        loadShader("drawNormalsPS", L"Shaders\\DrawNormals.hlsl", PixelShader, nullptr, "PS", "ps_5_1");
        loadShader("drawNormalsAlphaDropPS", L"Shaders\\DrawNormals.hlsl", PixelShader, alphaTestDefines, "PS", "ps_5_1");

        loadShader("ssaoVS", L"Shaders\\Ssao.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
        loadShader("ssaoPS", L"Shaders\\Ssao.hlsl", PixelShader, nullptr, "PS", "ps_5_1");

        loadShader("ssaoBlurVS", L"Shaders\\SsaoBlur.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
        loadShader("ssaoBlurPS", L"Shaders\\SsaoBlur.hlsl", PixelShader, nullptr, "PS", "ps_5_1");
    }
}

const std::shared_ptr<GShader>& RenderModeFactory::GetShader(const std::string& name)
{
    return shaders[name];
}
