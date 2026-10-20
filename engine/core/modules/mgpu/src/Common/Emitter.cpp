#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/Emitter.h"

#include "nau/mgpu/Utils/MathHelper.h"

//void Emitter::CompileGrassShaders()
//{
//	// Øåéäåð äëÿ ãåíåðàöèè òðàâû
//	generateShader = std::move(
//		std::make_shared<GShader>(L"Shaders\\GrassGenerate.hlsl", ComputeShader, nullptr, "CS", "cs_5_1"));
//	generateShader->LoadAndCompile();
//
//	// Øåéäåðû äëÿ îòðèñîâêè (åñëè íóæíû îòäåëüíî îò ÷àñòèö)
//	auto vertexShader = std::move(
//		std::make_shared<GShader>(L"Shaders\\GrassDraw.hlsl", VertexShader, nullptr, "VS", "vs_5_1"));
//	vertexShader->LoadAndCompile();
//
//	auto pixelShader = std::move(
//		std::make_shared<GShader>(L"Shaders\\GrassDraw.hlsl", PixelShader, nullptr, "PS", "ps_5_1"));
//	pixelShader->LoadAndCompile();
//
//	// Äëÿ òðàâû íå íóæåí geometry shader, èñïîëüçóåì âåðøèíû äëÿ ñîçäàíèÿ êâàäîâ
//}

void Emitter::PSOInitialize()
{
    if (renderPSO == nullptr)
    {
        CD3DX12_DESCRIPTOR_RANGE range[3];
        range[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);
        range[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 1);
        range[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(Atlas.size()), 2, 1);

        renderSignature = std::make_shared<GRootSignature>();
        renderSignature->AddConstantBufferParameter(0); // Object position		
        renderSignature->AddConstantBufferParameter(1); // World Data
        renderSignature->AddConstantParameter(sizeof(EmitterData) / sizeof(float), 0, 1); //EmitterData
        renderSignature->AddDescriptorParameter(&range[0], 1); //Particles
        renderSignature->AddDescriptorParameter(&range[1], 1); //Particles Render Index
        renderSignature->AddDescriptorParameter(&range[2], 1); //Particles Render Index
        renderSignature->Initialize(device);
    }

    if (computeSignature == nullptr)
    {
        CD3DX12_DESCRIPTOR_RANGE range[4];
        range[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        range[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
        range[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
        range[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);

        computeSignature = std::make_shared<GRootSignature>();
        computeSignature->AddConstantParameter(sizeof(EmitterData) / sizeof(float), 0); // EmitterData		
        computeSignature->AddDescriptorParameter(&range[0], 1);
        computeSignature->AddDescriptorParameter(&range[1], 1);
        computeSignature->AddDescriptorParameter(&range[2], 1);
        computeSignature->AddDescriptorParameter(&range[3], 1);
        computeSignature->Initialize(device, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
}
