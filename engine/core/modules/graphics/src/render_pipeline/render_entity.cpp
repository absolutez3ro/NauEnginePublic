// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "render_entity.h"

#include "nau/shaders/shader_globals.h"

void nau::RenderEntity::render(nau::math::Matrix4 viewProj) const
{
    const nau::math::Matrix4 mvpMatrix = viewProj * worldTransform;
    const nau::math::Matrix4 normalMatrix = math::transpose(math::inverse(worldTransform));

    nau::shader_globals::setVariable("vp", &viewProj);
    nau::shader_globals::setVariable("mvp", &mvpMatrix);
    nau::shader_globals::setVariable("worldMatrix", &worldTransform);
    nau::shader_globals::setVariable("normalMatrix", &normalMatrix);

    for (const auto& cbStruct : cbStructsData)
    {
        nau::shader_globals::setVariable(cbStruct.first, cbStruct.second.dataPtr);
    }

    NAU_ASSERT(material);
    material->bindPipeline("default");

    d3d::set_buffer(STAGE_VS, 0, nullptr);

    d3d::setvsrc(0, positionBuffer, sizeof(nau::math::float3));
    d3d::setvsrc(1, normalsBuffer, sizeof(nau::math::float3));
    d3d::setvsrc(2, texcoordsBuffer, sizeof(nau::math::float2));
    if (tangentsBuffer != nullptr)
    {
        d3d::setvsrc(3, tangentsBuffer, sizeof(nau::math::float4));
    }

    if (boneWeightsBuffer != nullptr && boneIndicesBuffer != nullptr)
    {
        d3d::setvsrc(4, boneWeightsBuffer, sizeof(nau::math::float4));
        d3d::setvsrc(5, boneIndicesBuffer, sizeof(nau::math::float4));
    }

    d3d::setind(indexBuffer);

    d3d::drawind(PRIM_TRILIST, startIndex, (endIndex - startIndex) / 3, 0);
}

void nau::RenderEntity::renderInstanced(nau::math::Matrix4 viewProj, Sbuffer* instanceData) const
{
    NAU_ASSERT(material);
    d3d::set_buffer(STAGE_VS, 0, instanceData);

    material->setProperty("instanced", "instanceBaseID", nau::math::Vector4(startInstance));
    material->bindPipeline("instanced");

    d3d::setvsrc(0, positionBuffer, sizeof(nau::math::float3));
    d3d::setvsrc(1, normalsBuffer, sizeof(nau::math::float3));
    d3d::setvsrc(2, texcoordsBuffer, sizeof(nau::math::float2));
    d3d::setvsrc(3, tangentsBuffer, sizeof(nau::math::float4));

    d3d::setind(indexBuffer);

    d3d::drawind_instanced(PRIM_TRILIST, startIndex, (endIndex - startIndex) / 3, 0, instancesCount, 0);
}

void nau::RenderEntity::renderZPrepass(const math::Matrix4& viewProj, MaterialAssetView* zPrepassMat) const
{
    const bool skinned = boneWeightsBuffer != nullptr && boneIndicesBuffer != nullptr;

    if (skinned)
    {
        shader_globals::setVariable("BonesTransforms", cbStructsData.at("BonesTransforms").dataPtr);
        shader_globals::setVariable("BonesNormalTransforms", cbStructsData.at("BonesNormalTransforms").dataPtr);

        prepareZPrepass("skinned", viewProj, zPrepassMat);
        d3d::setvsrc(0, positionBuffer, sizeof(math::float3));
        d3d::setvsrc(2, boneWeightsBuffer, sizeof(math::float4));
        d3d::setvsrc(3, boneIndicesBuffer, sizeof(math::IVector4));
    }
    else
    {
        prepareZPrepass("default", viewProj, zPrepassMat);
        d3d::setvsrc(0, positionBuffer, sizeof(math::float3));
    }

    d3d::setvsrc(1, normalsBuffer, sizeof(math::float3));
    d3d::setind(indexBuffer);
    d3d::drawind(PRIM_TRILIST, startIndex, (endIndex - startIndex) / 3, 0);
}

void nau::RenderEntity::renderZPrepassInstanced(const math::Matrix4& viewProj, MaterialAssetView* zPrepassMat) const
{
    shader_globals::setVariable("vp", &viewProj);
    prepareZPrepass("default", viewProj, zPrepassMat);

    d3d::setvsrc(0, positionBuffer, sizeof(math::float3));
    d3d::setvsrc(1, normalsBuffer, sizeof(math::float3));
    d3d::setind(indexBuffer);
    d3d::drawind_instanced(PRIM_TRILIST, startIndex, (endIndex - startIndex) / 3, 0, instancesCount, 0);
}

void nau::RenderEntity::prepareZPrepass(eastl::string_view pipeline, const math::Matrix4& viewProj, MaterialAssetView* zPrepassMat) const
{
    NAU_ASSERT(zPrepassMat);

    const nau::math::Matrix4 mvpMatrix = viewProj * worldTransform;
    const nau::math::Matrix4 normalMatrix = math::transpose(math::inverse(worldTransform));

    nau::shader_globals::setVariable("vp", &viewProj);
    nau::shader_globals::setVariable("mvp", &mvpMatrix);
    nau::shader_globals::setVariable("worldMatrix", &worldTransform);
    nau::shader_globals::setVariable("normalMatrix", &normalMatrix);
    auto instanceId = math::Vector4(startInstance);
    nau::shader_globals::setVariable("instanceBaseID", &instanceId);

    zPrepassMat->bindPipeline(pipeline);
}
