#pragma once

#include <memory>
#include "SimpleMath.h"
#include "nau/mgpu/Common/GModel.h"
#include "nau/mgpu/Common/ShaderBuffersData.h"
#include "nau/mgpu/Graphics/DirectXBuffers.h"
#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Graphics/GTexture.h"

using namespace DirectX::SimpleMath;

using namespace nau::PEPEngine;
using namespace Graphics;
using namespace Allocator;
using namespace Utils;

// Section 15 (B): GameObject, Transform, ModelRenderer and SkyBox of A are not transferred.
// This is the part of them the MGPU-ShadowMap scene needs.

// A: Transform::GetLocalMatrix after SetPosition / SetEulerRotate (degrees) / SetScale.
Matrix LocalMatrix(const Vector3& position, const Vector3& eulerAngles = Vector3::Zero,
                   const Vector3& scale = Vector3::One);

// A model on one device with a fixed world matrix: ModelRenderer of A, or SkyBox of A for the sky constructor.
class SceneRenderer
{
    ObjectConstants objectWorldData{};
    std::shared_ptr<ConstantUploadBuffer<ObjectConstants>> modelDataBuffer = nullptr;
    std::shared_ptr<GDevice> device;
    std::shared_ptr<GModel> model;

    bool isSkyBox = false;
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuTextureHandle{};

public:
    // A: ModelRenderer constructor + ModelRenderer::Update. The constants are written once: nothing moves.
    SceneRenderer(const std::shared_ptr<GDevice>& device, const std::shared_ptr<GModel>& model, const Matrix& world);

    // A: SkyBox constructor + SkyBox::Update.
    SceneRenderer(const std::shared_ptr<GDevice>& device, const std::shared_ptr<GModel>& model, const Matrix& world,
                  GTexture& skyMapTexture, GDescriptor* srvMemory, UINT offset = 0);

    // A: ModelRenderer::Draw / SkyBox::Draw.
    void Draw(const std::shared_ptr<GCommandList>& cmdList) const;
};
