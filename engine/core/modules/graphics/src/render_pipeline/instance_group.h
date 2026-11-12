// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <EASTL/vector.h>

#include <cstdlib>
#include <cstring>

#include "nau/3d/dag_drv3d.h"
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


    // Stage 1 of the MGPU optimisation (the CPU cost of the render lists): on by default; NAU_RENDER_OPTIMIZATIONS=0 switches
    // it off, to compare with the original behaviour in the same build.
    inline bool renderOptimizationsEnabled()
    {
        static const bool enabled = []
        {
            const char* const value = std::getenv("NAU_RENDER_OPTIMIZATIONS");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    struct InstanceInfo
    {
        InstanceID id;

        // Set through setWorldMatrix: the normal matrix follows it.
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

        // The normal matrix is kept here instead of being computed for every instance in every view of every frame.
        void setWorldMatrix(const nau::math::Matrix4& matrix)
        {
            worldMatrix = matrix;
            normalMatrix = nau::math::transpose(nau::math::inverse(matrix));
        }
    };


    // How a view chooses the levels of detail (RenderView::setLodSelection): the distance from the viewer is multiplied by
    // distanceScale (more than 1 for the far shadow cascades), and the shadow-only levels are taken by the shadow cascades.
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
