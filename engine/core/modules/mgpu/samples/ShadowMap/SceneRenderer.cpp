#include "SceneRenderer.h"

#include "nau/mgpu/Common/GMesh.h"
#include "nau/mgpu/Common/Material.h"
#include "nau/mgpu/Graphics/GCommandList.h"

Matrix LocalMatrix(const Vector3& position, const Vector3& eulerAngles, const Vector3& scale)
{
    const Quaternion rotate = Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(eulerAngles.y),
                                                                 DirectX::XMConvertToRadians(eulerAngles.x),
                                                                 DirectX::XMConvertToRadians(eulerAngles.z));

    return Matrix::CreateScale(scale) * Matrix::CreateFromQuaternion(rotate) * Matrix::CreateTranslation(position);
}

SceneRenderer::SceneRenderer(const std::shared_ptr<GDevice>& device, const std::shared_ptr<GModel>& model,
                             const Matrix& world) : device(device), model(model)
{
    modelDataBuffer = std::make_shared<ConstantUploadBuffer<ObjectConstants>>(
        device, model->GetMeshesCount(), L"Object Constants");

    // A: Transform::TextureTransform
    objectWorldData.TextureTransform = Matrix::CreateScale(Vector3::One).Transpose();
    objectWorldData.World = (world * model->scaleMatrix).Transpose();
    for (UINT i = 0; i < model->GetMeshesCount(); ++i)
    {
        auto material = model->GetMeshMaterial(i);
        if (material != nullptr)
        {
            objectWorldData.MaterialIndex = material->GetIndex();
        }
        modelDataBuffer->CopyData(i, objectWorldData);
    }
}

SceneRenderer::SceneRenderer(const std::shared_ptr<GDevice>& device, const std::shared_ptr<GModel>& model,
                             const Matrix& world, GTexture& skyMapTexture, GDescriptor* srvMemory,
                             const UINT offset) : device(device), model(model), isSkyBox(true)
{
    modelDataBuffer = std::make_shared<ConstantUploadBuffer<ObjectConstants>>(
        device, model->GetMeshesCount(), L"Object Constants");

    gpuTextureHandle = srvMemory->GetGPUHandle(offset);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    auto desc = skyMapTexture.GetD3D12Resource()->GetDesc();
    srvDesc.Format = GetSRGBFormat(desc.Format);
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = desc.MipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;
    skyMapTexture.CreateShaderResourceView(&srvDesc, srvMemory, offset);

    // A: Transform::TextureTransform
    objectWorldData.TextureTransform = Matrix::CreateScale(Vector3::One).Transpose();
    objectWorldData.World = (world * model->scaleMatrix).Transpose();
    for (UINT i = 0; i < model->GetMeshesCount(); ++i)
    {
        modelDataBuffer->CopyData(i, objectWorldData);
    }
}

void SceneRenderer::Draw(const std::shared_ptr<GCommandList>& cmdList) const
{
    if (isSkyBox)
    {
        cmdList->GetGraphicsCommandList()->SetGraphicsRootDescriptorTable(StandardShaderSlot::SkyMap,
                                                                          gpuTextureHandle);
    }

    for (UINT i = 0; i < model->GetMeshesCount(); ++i)
    {
        const auto mesh = model->GetMesh(i);

        cmdList->SetRootConstantBufferView(StandardShaderSlot::ObjectData,
                                           *modelDataBuffer, i);
        mesh->Draw(cmdList);
    }
}
