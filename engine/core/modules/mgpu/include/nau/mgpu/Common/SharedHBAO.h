#pragma once
#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Common/GCrossAdapterResource.h"
#include "nau/mgpu/Graphics/GraphicPSO.h"
#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Graphics/GRenderTarger.h"
#include "nau/mgpu/Graphics/GTexture.h"
#include "nau/mgpu/Utils/MathHelper.h"
#include "nau/mgpu/Common/RenderModeFactory.h"
#include "nau/mgpu/Common/ShaderBuffersData.h"
#include "nau/mgpu/Common/SharedSSAO.h"

using namespace DirectX::SimpleMath;

using namespace nau::PEPEngine;
using namespace Graphics;
using namespace Allocator;
using namespace Utils;

struct alignas(sizeof(Vector4)) HBAOConstants
{
    Matrix ProjMatrix;
    Matrix InvProjMatrix; // offset:    0
    Vector4 Resolution; // offset:   64
    Vector2 ClipInfo; // offset:   80
    float TraceRadius; // offset:   88
    float MaxRadiusPixels; // offset:   92
    float DiscardDistance; // offset:   96
    Vector3 Padding; // offset:  100
};

class HBAOResources final : public virtual SSAOResources
{
    GDescriptor ambientMapUAV;

    ComputePSO pso;

    void RebuildDescriptors() const override;

    void InitializeRS() override;

    void BuildPSO(const D3D12_INPUT_LAYOUT_DESC& layout) override;

public:
    const GDescriptor* GetAmbientMapUAV() const { return &ambientMapUAV; }

    void Initialize(const std::shared_ptr<GDevice>& Device, const D3D12_INPUT_LAYOUT_DESC& layout) override;
    const ComputePSO& GetPso() const { return pso; }
};

class SharedHBAO
{
    HBAOResources primeResources;
    HBAOResources secondResources;
    SSAOCrossResources crossResources;

    UINT RenderTargetWidth;
    UINT RenderTargetHeight;

public:
    const HBAOResources& GetPrimeResources() const { return primeResources; }
    const HBAOResources& GetSecondResources() const { return secondResources; }
    const SSAOCrossResources& GetCrossResources() const { return crossResources; }

    void Initialize(const std::shared_ptr<GDevice>& PrimeDevice, const std::shared_ptr<GDevice>& SecondDevice,
                    const D3D12_INPUT_LAYOUT_DESC& layout, UINT width, UINT height);

    void OnResize(UINT width, UINT height);
};
