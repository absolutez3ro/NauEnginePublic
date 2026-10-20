#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/SharedHBAO.h"


void HBAOResources::RebuildDescriptors() const
{
    SSAOResources::RebuildDescriptors();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uavDesc.Format = AmbientMapFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.PlaneSlice = 0;
    uavDesc.Texture2D.MipSlice = 0;
    GetAmbientMap().CreateUnorderedAccessView(&uavDesc, &ambientMapUAV);
}

void HBAOResources::InitializeRS()
{
    aoRootSignature = std::make_shared<GRootSignature>();

    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable2;
    texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);

    aoRootSignature->AddConstantBufferParameter(0);
    aoRootSignature->AddDescriptorParameter(&texTable0, 1);
    aoRootSignature->AddDescriptorParameter(&texTable1, 1);
    aoRootSignature->AddDescriptorParameter(&texTable2, 1);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressW
        0.0f,
        0,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
    {
        pointClamp, linearClamp, depthMapSam, linearWrap
    };

    for (auto&& sampler : staticSamplers)
    {
        aoRootSignature->AddStaticSampler(sampler);
    }
    aoRootSignature->Initialize(device);
}

void HBAOResources::Initialize(const std::shared_ptr<GDevice>& Device, const D3D12_INPUT_LAYOUT_DESC& layout)
{
    SSAOResources::Initialize(Device, layout);

    ambientMapUAV = ambientMapSRV.Offset(1);
}

void HBAOResources::BuildPSO(const D3D12_INPUT_LAYOUT_DESC& layout)
{
}

void SharedHBAO::Initialize(const std::shared_ptr<GDevice>& PrimeDevice, const std::shared_ptr<GDevice>& SecondDevice, const D3D12_INPUT_LAYOUT_DESC& layout, UINT width, UINT height)
{
    primeResources.Initialize(PrimeDevice, layout);
    primeResources.OnResize(width, height);

    secondResources.Initialize(SecondDevice, layout);
    secondResources.OnResize(width, height);

    crossResources.Initialize(primeResources, PrimeDevice, SecondDevice);
    crossResources.OnResize(width, height);
    OnResize(width, height);
}

void SharedHBAO::OnResize(UINT newWidth, UINT newHeight)
{
    if (RenderTargetWidth == newWidth && RenderTargetHeight == newHeight)
        return;

    RenderTargetWidth = newWidth;
    RenderTargetHeight = newHeight;

    primeResources.OnResize(newWidth, newHeight);
    secondResources.OnResize(newWidth, newHeight);
    crossResources.OnResize(newWidth, newHeight);
}
