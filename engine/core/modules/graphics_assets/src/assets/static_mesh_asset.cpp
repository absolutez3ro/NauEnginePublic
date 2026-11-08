// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "graphics_assets/static_mesh_asset.h"

#include "nau/assets/mesh_asset_accessor.h"


namespace nau
{
    async::Task<nau::Ptr<StaticMeshAssetView>> StaticMeshAssetView::createFromAssetAccessor(nau::Ptr<> accessor)
    {
        using namespace nau::async;

        NAU_ASSERT(accessor);

        ASYNC_SWITCH_EXECUTOR(async::Executor::getDefault());

        auto& meshAccessor = accessor->as<IMeshAssetAccessor&>();
        auto meshAssetView = rtti::createInstance<StaticMeshAssetView>();

        meshAssetView->m_mesh = co_await StaticMesh::createFromStaticMeshAccessor(meshAccessor);

        co_return meshAssetView;
    }

    Sbuffer* StaticMeshAssetView::getPositionsBuffer() const
    {
        return m_mesh->getLod(0).m_positionsBuffer;
    }

    Sbuffer* StaticMeshAssetView::getNormalsBuffer() const
    {
        return m_mesh->getLod(0).m_normalsBuffer;
    }

    Sbuffer* StaticMeshAssetView::getTangentsBuffer() const
    {
        return m_mesh->getLod(0).m_tangentsBuffer;
    }

    Sbuffer* StaticMeshAssetView::getTexcoordsBuffer() const
    {
        return m_mesh->getLod(0).m_texCoordsBuffer;
    }

    Sbuffer* StaticMeshAssetView::getIndexBuffer() const
    {
        return m_mesh->getLod(0).m_indexBuffer;
    }

    unsigned StaticMeshAssetView::getIndexCount() const
    {
        return m_mesh->getLod(0).m_indexCount;
    }

    unsigned StaticMeshAssetView::getVertexCount() const
    {
        return m_mesh->getLod(0).m_vertexCount;
    }

    void StaticMeshAssetView::enumerateMeshTriangles(
        Functor<void(const nau::math::vec3&, const nau::math::vec3&, const nau::math::vec3&)> sink) const
    {
        const StaticMeshLod& lod = m_mesh->getLod(0);

        const auto enumerate = [&lod, &sink](const auto* indices)
        {
            const auto point = [&lod](const size_t index)
            {
                const nau::math::float3& p = lod.m_cpuPositions[index];
                return nau::math::vec3{p.x, p.y, p.z};
            };

            for (size_t i = 0; i + 2 < lod.m_indexCount; i += 3)
            {
                eastl::invoke(sink, point(indices[i]), point(indices[i + 1]), point(indices[i + 2]));
            }
        };

        if (lod.m_indices32)
        {
            enumerate(reinterpret_cast<const uint32_t*>(lod.m_cpuIndices.data()));
        }
        else
        {
            enumerate(reinterpret_cast<const uint16_t*>(lod.m_cpuIndices.data()));
        }
    }

}  // namespace nau
