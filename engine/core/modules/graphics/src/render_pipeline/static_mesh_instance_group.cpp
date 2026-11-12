// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "static_mesh_instance_group.h"

#include <EASTL/algorithm.h>
#include <EASTL/fixed_vector.h>

#include <cmath>

#include "graphics_assets/static_mesh_asset.h"
#include "nau/string/hash.h"

namespace nau
{

    StaticMeshInstanceGroup::StaticMeshInstanceGroup(nau::ReloadableAssetView::Ptr mesh) :
        m_staticMesh(mesh)
    {
    }

    InstanceInfo StaticMeshInstanceGroup::addInstance(const nau::math::Matrix4& matrix)
    {
        auto inst = createInfo(matrix);
        addInstance(inst);

        return inst;
    }

    void StaticMeshInstanceGroup::addInstance(const InstanceInfo& inst)
    {
        m_instances[inst.id] = inst;
    }

    InstanceID StaticMeshInstanceGroup::reserveID()
    {
        InstanceID newID = freeInstanceId++;
        return newID;
    }

    InstanceInfo StaticMeshInstanceGroup::createInfo(const nau::math::Matrix4& matrix)
    {
        InstanceID newID = freeInstanceId++;
        auto info = InstanceInfo();
        info.id = newID;
        info.isVisible = true;
        info.setWorldMatrix(matrix);
        Ptr<StaticMeshAssetView> meshView;
        m_staticMesh->getTyped<StaticMeshAssetView>(meshView);
        info.localSphere = meshView->getMesh()->getLod0BSphere();
        info.worldSphere = meshView->getMesh()->getLod0BSphere();
        info.worldSphere.c = info.worldMatrix.getTranslation();

        return info;
    }

    RenderEntity nau::StaticMeshInstanceGroup::createRenderEntity()
    {
        return RenderEntity();
    }


    RenderList::Ptr nau::StaticMeshInstanceGroup::createRenderList(const nau::math::Vector3& viewerPosition,
        eastl::function<bool(const InstanceInfo&)>& filterFunc,
        eastl::function<bool(const nau::MaterialAssetView::Ptr)>& materialFilter,
        const LodSelection& lodSelection)
    {
        NAU_FATAL(m_staticMesh);

        RenderList::Ptr ret = eastl::make_shared<RenderList>();

        Ptr<StaticMeshAssetView> meshView;
        m_staticMesh->getTyped<StaticMeshAssetView>(meshView);
        const nau::StaticMesh& mesh = *meshView->getMesh();
        const bool hasLods = mesh.getLodsCount() > 1;
        eastl::vector<eastl::vector<eastl::map<size_t /*material name*/, uint32_t /*entityIndex*/>>> lodSlotMats(mesh.getLodsCount());

        // Materials resolved once per call (renderOptimizationsEnabled): getTyped takes a lock and does a cast, and the result
        // of the material filter depends on the material only.
        const bool optimized = renderOptimizationsEnabled();
        struct ResolvedMaterial
        {
            const ReloadableAssetView* source = nullptr;
            nau::Ptr<nau::MaterialAssetView> material;
            size_t nameHash = 0;
            bool passes = false;
        };
        eastl::fixed_vector<ResolvedMaterial, 8, true> resolvedMaterials;
        const auto resolveMaterial = [&](const ReloadableAssetView::Ptr& source) -> const ResolvedMaterial&
        {
            for (const ResolvedMaterial& resolved : resolvedMaterials)
            {
                if (resolved.source == source.get())
                {
                    return resolved;
                }
            }
            ResolvedMaterial& resolved = resolvedMaterials.push_back();
            resolved.source = source.get();
            source->getTyped<MaterialAssetView>(resolved.material);
            resolved.passes = resolved.material && materialFilter(resolved.material);
            resolved.nameHash = resolved.passes ? resolved.material->getNameHash() : 0;
            return resolved;
        };

        for (const auto& [id, info] : m_instances)
        {
            if (!info.isVisible)
            {
                continue;
            }

            if (!filterFunc(info))
            {
                continue;
            }

            // Level of detail by the distance from the viewer, in the units of the mesh: the scale of the instance is divided
            // out. Every view gets the position of the main camera, so the shadows are cast by the levels that are seen; the
            // far shadow cascades scale the distance up, their texels are larger than the details dropped, and the shadow
            // cascades take the shadow-only levels (lodSelection).
            uint32_t lodLevel = 0;
            if (hasLods)
            {
                const float scale = std::sqrt(float(lengthSqr(info.worldMatrix.getCol0().getXYZ())));
                const float distance = std::sqrt(float(lengthSqr(info.worldMatrix.getTranslation() - viewerPosition)));
                lodLevel = mesh.selectLod(distance * lodSelection.distanceScale / eastl::max(scale, 1e-6f),
                                          lodSelection.shadowLevels);
            }

            const nau::StaticMeshLod& lod = mesh.getLod(lodLevel);

            auto& slotMats = lodSlotMats[lodLevel];

            if (slotMats.empty())
            {
                slotMats.resize(lod.m_materialSlots.size());
            }

            // iterate through slots
            for (size_t slotInd = 0; slotInd < lod.m_materialSlots.size(); slotInd++)
            {
                const nau::MaterialSlot& slot = lod.m_materialSlots[slotInd];
                uint64_t lodSlot = (uint64_t(lodLevel) << 32) | uint64_t(slotInd);

                nau::Ptr<nau::MaterialAssetView> material;
                auto materialOverride = info.overrideInfo.find(lodSlot);
                if (materialOverride == info.overrideInfo.end() && lodLevel != 0)
                {
                    // StaticMeshComponent::setMaterial overrides level 0: the simplified levels use the same material.
                    materialOverride = info.overrideInfo.find(uint64_t(slotInd));
                }

                const ReloadableAssetView::Ptr& materialSource = materialOverride != info.overrideInfo.end()
                                                                     ? materialOverride->second.material
                                                                     : slot.m_material;
                size_t matNameHash = 0;
                if (optimized)
                {
                    const ResolvedMaterial& resolved = resolveMaterial(materialSource);
                    if (!resolved.passes)
                    {
                        continue;
                    }
                    material = resolved.material;
                    matNameHash = resolved.nameHash;
                }
                else
                {
                    materialSource->getTyped<MaterialAssetView>(material);
                    if (!materialFilter(material))
                    {
                        continue;
                    }
                    matNameHash = material->getNameHash(); // TODO: cache this inside material
                }
                auto& mats = slotMats[slotInd];

                if (!mats.count(matNameHash))
                {
                    mats[matNameHash] = ret->getEntitiesCount();

                    nau::RenderEntity& ent = ret->emplaceBack();
                    ent.positionBuffer = lod.m_positionsBuffer;
                    ent.normalsBuffer = lod.m_normalsBuffer;
                    ent.texcoordsBuffer = lod.m_texCoordsBuffer;
                    ent.tangentsBuffer = lod.m_tangentsBuffer;
                    ent.indexBuffer = lod.m_indexBuffer;
                    ent.meshLod = &lod;
                    ent.startInstance = 0;
                    ent.instancesCount = 0;
                    ent.instanceData = {};
                    ent.tags = {};

                    ent.worldTransform = info.worldMatrix;  // keep first world matrix
                    ent.startIndex = slot.m_startIndex;
                    ent.endIndex = slot.m_endIndex;
                    ent.material = material;
                }

                uint32_t entInd = mats[matNameHash];
                nau::RenderEntity& entity = (*ret)[entInd];

                entity.instancesCount++;
                if (optimized)
                {
                    entity.instanceData.emplace_back(info.worldMatrix, info.normalMatrix, info.uid, info.isHighlighted);
                }
                else
                {
                    const nau::math::Matrix4 normalMatrix = math::transpose(math::inverse(info.worldMatrix));
                    entity.instanceData.emplace_back(info.worldMatrix, normalMatrix, info.uid, info.isHighlighted);
                }
            }
        }

        return ret;
    }

    void StaticMeshInstanceGroup::clearPendingInstances()
    {
        eastl::erase_if(m_instances, [](const auto& pair)
        {
            return pair.second.toDelete;
        });
    }

    void StaticMeshInstanceGroup::removeInstance(InstanceID instID)
    {
        NAU_ASSERT(contains(instID));
        m_instances.erase(instID);
    }

    bool StaticMeshInstanceGroup::contains(InstanceID instID) const
    {
        return m_instances.contains(instID);
    }

    size_t StaticMeshInstanceGroup::getInstancesCount() const
    {
        return m_instances.size();
    }

    InstanceInfo& StaticMeshInstanceGroup::getInstance(InstanceID instID)
    {
        return m_instances[instID];
    }

}  // namespace nau
