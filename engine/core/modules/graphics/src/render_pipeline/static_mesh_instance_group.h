// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <graphics_assets/static_mesh_asset.h>

#include "graphics_assets/static_meshes/static_mesh.h"
#include "instance_group.h"
#include "nau/3d/dag_drv3d.h"
#include "render_list.h"

namespace nau
{
    class StaticMeshInstanceGroup : public IInstanceGroup
    {
    public:

        StaticMeshInstanceGroup(nau::ReloadableAssetView::Ptr mesh);

        InstanceInfo addInstance(const nau::math::Matrix4& matrix);
        void addInstance(const InstanceInfo& inst);

        InstanceID reserveID();
        InstanceInfo createInfo(const nau::math::Matrix4& matrix);

        // Inherited via IInstanceGroup
        size_t getInstancesCount() const override;

        InstanceInfo& getInstance(InstanceID instID) override;
        void removeInstance(InstanceID instID) override;
        bool contains(InstanceID instID) const override;

        RenderEntity createRenderEntity() override;
        RenderList::Ptr createRenderList(const nau::math::Vector3& viewerPosition,
            eastl::function<bool(const InstanceInfo&)>& filterFunc,
            eastl::function<bool(const nau::MaterialAssetView::Ptr)>& materialFilter,
            const LodSelection& lodSelection) override;

        void clearPendingInstances();

        inline nau::math::BSphere3 getMeshBSphereLod0()
        {
            Ptr<StaticMeshAssetView> meshView;
            m_staticMesh->getTyped<StaticMeshAssetView>(meshView);
            return meshView->getMesh()->getLod0BSphere();
        }

    protected:
        nau::ReloadableAssetView::Ptr m_staticMesh;
        eastl::unordered_map<InstanceID, InstanceInfo> m_instances;
        std::atomic<InstanceID> freeInstanceId = 0;
    };

} // namespace nau
