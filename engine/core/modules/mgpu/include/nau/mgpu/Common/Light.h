#pragma once
#include "d3d12.h"
#include "SimpleMath.h"
#include "nau/mgpu/Allocator/MemoryAllocator.h"
#include <memory>

#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Common/ShaderBuffersData.h"


using namespace nau::PEPEngine;
using namespace Graphics;
using namespace Allocator;
using namespace Utils;


// Base Component from A is not transferred (B has no GameObject): Update is public and called by the owner;
// lightData.Position is not taken from a GameObject transform.
class Light
{
    const DirectX::XMFLOAT3 spotDirectionalDirection = {0.0f, -1.0f, 0.0f};
    int NumFramesDirty = globalCountFrameResources;
    LightData lightData{};

    DirectX::XMFLOAT3 strength = {0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 direction = {0, 0, 0};
    float falloffStart = 1.0f;
    float falloffEnd = 10.0f;
    float spotPower = 64.0f;
    LightType type;

    Matrix view = Matrix::Identity;
    Matrix projection = Matrix::Identity;

public:
    void Update();

    LightData GetData() const;

    LightType Type() const;

    Light(LightType type = Directional);;

    DirectX::XMFLOAT3 Direction() const;

    void Direction(DirectX::XMFLOAT3 direct);

    DirectX::XMFLOAT3 Strength() const;

    void Strength(DirectX::XMFLOAT3 strength);

    void FalloffStart(float start);

    float FalloffStart() const;

    void FalloffEnd(float end);

    float FalloffEnd() const;

    void SpotPower(float power);

    float SpotPower() const;
};
