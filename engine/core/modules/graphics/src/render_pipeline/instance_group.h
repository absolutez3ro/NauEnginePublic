// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <EASTL/vector.h>

#include "nau/3d/dag_drv3d.h"
#include "../graphics_settings.h"
#include "nau/math/dag_bounds3.h"
#include "graphics_assets/material_asset.h"
#include "render_entity.h"
#include "render_list.h"


namespace nau
{
    using InstanceID = uint64_t;

    struct MaterialOverrideInfo
    {
        uint64_t lodSlot;
        ReloadableAssetView::Ptr material;
    };


    inline bool renderOptimizationsEnabled()
    {
        return getGraphicsSettings().renderOptimizations;
    }

    struct InstanceInfo
    {
        InstanceID id;

        nau::math::Matrix4 worldMatrix;
        nau::math::Matrix4 normalMatrix = nau::math::Matrix4::identity();
        nau::math::BSphere3 worldSphere;
        nau::math::BSphere3 localSphere;

        eastl::map<uint64_t, MaterialOverrideInfo> overrideInfo;
        bool toDelete = false;

        // RenderParameters
        RenderTags tags;
        bool isVisible = true;
        bool isCastShadow = true;
        bool isHighlighted = false;

        nau::Uid uid;

        void setWorldMatrix(const nau::math::Matrix4& matrix)
        {
            worldMatrix = matrix;
            normalMatrix = nau::math::transpose(nau::math::inverse(matrix));
        }
    };


    struct LodSelection
    {
        float distanceScale = 1.0f;
        bool shadowLevels = false;
    };

    class IInstanceGroup
    {
    public:
        virtual size_t getInstancesCount() const = 0;
        virtual InstanceInfo& getInstance(InstanceID instID) = 0;
        virtual void removeInstance(InstanceID instID) = 0;
        virtual bool contains(InstanceID instID) const = 0;
        virtual RenderEntity createRenderEntity() = 0;
        virtual RenderList::Ptr createRenderList(const nau::math::Vector3& viewerPosition, 
            eastl::function<bool(const InstanceInfo&)>& filterFunc,
            eastl::function<bool(const nau::MaterialAssetView::Ptr)>& materialFilter,
            const LodSelection& lodSelection) = 0;

    protected:
        RenderTags tags;
    };

} // namespace nau
