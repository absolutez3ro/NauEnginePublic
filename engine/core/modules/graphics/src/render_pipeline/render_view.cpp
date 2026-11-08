// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "render_view.h"

#include "nau/shaders/shader_globals.h"

#include <EASTL/functional.h>


nau::RenderView::RenderView(eastl::string_view viewName) :
    m_viewName(viewName)
{
    if (!nau::shader_globals::containsName("mvp"))
    {
        nau::shader_globals::addVariable("mvp", sizeof(math::Matrix4));
    }
    if (!nau::shader_globals::containsName("vp"))
    {
        nau::shader_globals::addVariable("vp", sizeof(math::Matrix4));
    }
    if (!nau::shader_globals::containsName("normalMatrix"))
    {
        nau::shader_globals::addVariable("normalMatrix", sizeof(math::Matrix4));
    }
    if (!nau::shader_globals::containsName("worldMatrix"))
    {
        nau::shader_globals::addVariable("worldMatrix", sizeof(math::Matrix4));
    }
    if (!nau::shader_globals::containsName("uid"))
    {
        const auto uid = math::IVector4{};
        nau::shader_globals::addVariable("uid", sizeof(math::IVector4), &uid);
    }

    m_instanceFilter = eastl::function<bool(const InstanceInfo&)>([this](const InstanceInfo& info) 
        {
            return m_frustum.testSphere(info.worldSphere) != 0;
        });

    m_materialFilter = eastl::function<bool(const MaterialAssetView::Ptr)>([](const MaterialAssetView::Ptr material) 
        {
            nau::BlendMode mode = material->getBlendMode("default");
            return mode == nau::BlendMode::Opaque || mode == nau::BlendMode::Masked;
        });
}

nau::RenderView::~RenderView()
{
    if (m_instanceData)
    {
        m_instanceData->destroy();
        m_instanceData = nullptr;
    }
}

void nau::RenderView::addRenderList(RenderList::Ptr list)
{
    if (list)
    {
        m_lists.emplace_back(list);
    }
}

void nau::RenderView::clearLists()
{
    m_lists.clear();
}

namespace
{
    // Static meshes take the instanced pipelines even with a single instance. The depth prepass reads the instance buffer
    // in both of its paths, while the non-instanced gbuffer pipeline transforms with the mvp computed on the CPU: the depth
    // came out slightly different and failed the depth test of the gbuffer pass in places (black pixels). Levels of detail
    // split the instances of a mesh by distance, so entities with one instance are common.
    bool drawInstanced(const nau::RenderEntity& ent)
    {
        if (!ent.instancingSupported)
        {
            return false;
        }
        return ent.instancesCount != 1 || (ent.material && ent.material->hasPipeline("instanced"));
    }
}  // namespace

void nau::RenderView::render(const nau::math::Matrix4& vp) const
{
    for (auto& list : m_lists)
    {
        for (auto& ent : list->getEntities())
        {
            ent.render(vp);
        }
    }
}

void nau::RenderView::renderInstanced(const nau::math::Matrix4& vp) const
{
    if (m_instanceData == nullptr)
    {
        return;
    }

    nau::shader_globals::setVariable("vp", &vp);

    for (auto& list : m_lists)
    {
        for (auto& ent : list->getEntities())
        {
            if (!drawInstanced(ent))
            {
                ent.render(vp);
            }
            else
            {
                ent.renderInstanced(vp, m_instanceData);
            }
        }
    }
}

void nau::RenderView::renderZPrepass(const nau::math::Matrix4& vp, nau::MaterialAssetView* zPrepassMat) const
{
    if (m_instanceData == nullptr)
    {
        return;
    }

    NAU_ASSERT(zPrepassMat);
    zPrepassMat->setRoBuffer("default", "instanceBuffer", m_instanceData);
    zPrepassMat->setRoBuffer("skinned", "instanceBuffer", m_instanceData);
    if (zPrepassMat->hasPipeline("masked"))
    {
        zPrepassMat->setRoBuffer("masked", "instanceBuffer", m_instanceData);
    }

    for (auto& list : m_lists)
    {
        for (auto& ent : list->getEntities())
        {
            if (!drawInstanced(ent))
            {
                ent.renderZPrepass(vp, zPrepassMat);
            }
            else
            {
                ent.renderZPrepassInstanced(vp, zPrepassMat);
            }
        }
    }
}

void nau::RenderView::renderOutlineMask(const nau::math::Matrix4& vp, nau::MaterialAssetView* zPrepassMat) const
{
    if (m_instanceData == nullptr)
    {
        return;
    }

    NAU_ASSERT(zPrepassMat);
    zPrepassMat->setRoBuffer("default", "instanceBuffer", m_instanceData);

    for (auto& list : m_lists)
    {
        for (auto& ent : list->getEntities())
        {
            auto highlightedIt = std::find_if(ent.instanceData.begin(), ent.instanceData.end(),
                                              [](auto& data) -> bool
            {
                return data.isHighlighted;
            });
            if (highlightedIt == ent.instanceData.end())
            {
                continue;
            }
            if (!drawInstanced(ent))
            {
                ent.renderZPrepass(vp, zPrepassMat);
            }
            else
            {
                ent.renderZPrepassInstanced(vp, zPrepassMat);
            }
        }
    }
}

void nau::RenderView::updateFrustum(const nau::math::Matrix4& vp)
{
    m_frustum = nau::math::NauFrustum(vp);
}

void nau::RenderView::prepareInstanceData()
{
    uint32_t instsCount = 0;
    for (auto& list : m_lists)
    {
        for (const auto& ent : list->getEntities())
        {
            instsCount += ent.instancesCount;
        }
    }

    if (instsCount == 0)
    {
        return;
    }

    eastl::vector<nau::RenderEntity::InstanceData> instDataVec;
    instDataVec.reserve(instsCount);

    for (auto& list : m_lists)
    {
        for (auto& ent : list->getEntities())
        {
            ent.startInstance = instDataVec.size();
            instDataVec.insert(instDataVec.end(), ent.instanceData.begin(), ent.instanceData.end());
        }
    }

    if (m_maxInstancesCount < instsCount)
    {
        m_maxInstancesCount = instsCount;

        if (m_instanceData)
        {
            m_instanceData->destroy();
        }

        m_instanceData = d3d::create_sbuffer(sizeof(nau::RenderEntity::InstanceData), m_maxInstancesCount, SBCF_BIND_SHADER_RES | SBCF_MISC_STRUCTURED | SBCF_DYNAMIC, 0, u8"inst buf");
    }
    NAU_ASSERT(m_instanceData);

    bool isUpdated = m_instanceData->updateData(0, sizeof(nau::RenderEntity::InstanceData) * instsCount, instDataVec.data(), VBLOCK_WRITEONLY | VBLOCK_DISCARD);
    NAU_ASSERT(isUpdated);
}

bool nau::RenderView::containsTag(RenderTag tag)
{
    return m_tags.count(tag);
}

void nau::RenderView::addTag(RenderTag tag)
{
    m_tags.insert(tag);
}

void nau::RenderView::removeTag(RenderTag tag)
{
    m_tags.erase(tag);
}

void nau::RenderView::setUserData(void* userData)
{
    m_userData = userData;
}

void* nau::RenderView::getUserData()
{
    return m_userData;
}

eastl::function<bool(const nau::InstanceInfo&)>& nau::RenderView::getInstanceFilter()
{
    return m_instanceFilter;
}

void nau::RenderView::setInstanceFilter(eastl::function<bool(const InstanceInfo&)>& filter)
{
    m_instanceFilter = filter;
}

eastl::function<bool(const nau::MaterialAssetView::Ptr)>& nau::RenderView::getMaterialFilter()
{
    return m_materialFilter;
}

void nau::RenderView::setMaterialFilter(eastl::function<bool(const MaterialAssetView::Ptr)>& filter)
{
    m_materialFilter = filter;
}

