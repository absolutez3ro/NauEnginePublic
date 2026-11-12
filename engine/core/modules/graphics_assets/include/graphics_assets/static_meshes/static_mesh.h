// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "nau/rtti/rtti_impl.h"
#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_resPtr.h"
#include "nau/assets/asset_ref.h"
#include "nau/assets/mesh_asset_accessor.h"
#include "nau/async/task_base.h"
#include "graphics_assets/material_asset.h"
#include "nau/math/dag_bounds3.h"


namespace nau
{
    struct StaticMeshDescriptor
    {
        StaticMeshAssetRef staticMeshRef;
    };


    struct MaterialSlot
    {
        uint32_t m_startIndex;
        uint32_t m_endIndex;

        ReloadableAssetView::Ptr m_material;
    };


    struct StaticMeshLod
    {
        // Owned by the StaticMesh of the level, which frees them (~StaticMesh).
        Sbuffer* m_positionsBuffer = nullptr; // TODO: change to ResPtr's
        Sbuffer* m_normalsBuffer = nullptr;
        Sbuffer* m_tangentsBuffer = nullptr;
        Sbuffer* m_texCoordsBuffer = nullptr;

        Sbuffer* m_indexBuffer = nullptr;

        uint32_t m_indexCount;
        uint32_t m_vertexCount;

        // The GPU buffers live in video memory and are not read back. Copies for the code that needs the geometry on the
        // CPU (physics, the shadow casters of the second GPU).
        eastl::vector<nau::math::float3> m_cpuPositions;
        eastl::vector<nau::math::float2> m_cpuTexCoords;
        eastl::vector<uint8_t> m_cpuIndices; // uint16_t, or uint32_t when m_indices32
        bool m_indices32 = false;

        nau::math::BBox3 m_localBBox;

        eastl::vector<MaterialSlot> m_materialSlots;
    };


    class NAU_GRAPHICSASSETS_EXPORT StaticMesh : public virtual IRefCounted
    {
        NAU_CLASS_(nau::StaticMesh, IRefCounted);
    public:

        using Ptr = nau::Ptr<StaticMesh>;

        virtual ~StaticMesh();
        StaticMesh();

        const nau::StaticMeshLod& getLod(uint32_t lodInd) const;
        uint32_t getLodsCount() const;

        // The level of detail for a viewer at this distance from the origin of an instance, in the units of the mesh
        // (the distance divided by the scale of the instance). The shadow-only levels are chosen for shadowLevels only (the
        // shadow cascades). See IMeshLodsAccessor.
        uint32_t selectLod(float distance, bool shadowLevels = false) const;

        inline const nau::math::BSphere3& getLod0BSphere() const
        {
            return m_localBSphere;
        }

    public:
        static async::Task<nau::Ptr<StaticMesh>> createFromStaticMeshAccessor(IMeshAssetAccessor& accessor);

        static bool createFromGeneratedData();

    protected:
        StaticMeshDescriptor m_meshDescriptor;

        nau::math::BSphere3 m_localBSphere;

        eastl::vector<StaticMeshLod> lods;
        eastl::vector<float> m_lodDistances; // of the levels 1, 2, ...
        eastl::vector<bool> m_lodShadowOnly; // the same levels: drawn by the shadow cascades only (always the last ones)
        eastl::vector<float> m_lodsScreenSpaceError;
        float cullDistance;
    };
}
