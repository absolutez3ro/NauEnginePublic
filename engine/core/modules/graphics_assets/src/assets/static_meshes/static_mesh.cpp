// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "graphics_assets/static_meshes/static_mesh.h"

#include <cmath>
#include <cstring>

#include "nau/3d/dag_d3dResource.h"
#include "nau/async/task.h"
#include "nau/diag/logging.h"

nau::StaticMesh::StaticMesh()
{
}

// The buffers of the levels were never freed before: every unloaded scene kept its geometry in video memory. Freed here and
// not in StaticMeshLod, which is copied around in the vector of the levels. The driver deletes a buffer once the frames
// that may still use it have completed.
nau::StaticMesh::~StaticMesh()
{
    for (StaticMeshLod& lod : lods)
    {
        del_d3dres(lod.m_positionsBuffer);
        del_d3dres(lod.m_normalsBuffer);
        del_d3dres(lod.m_tangentsBuffer);
        del_d3dres(lod.m_texCoordsBuffer);
        del_d3dres(lod.m_indexBuffer);
    }
}

namespace
{
    // Tangents of the vertices. A triangle without usable texture coordinates leaves the tangents it touches as they are.
    template <typename Index>
    void computeTangents(const Index* indices,
                         size_t indexCount,
                         const eastl::vector<nau::math::float3>& positions,
                         const eastl::vector<nau::math::float3>& normals,
                         const eastl::vector<nau::math::float2>& uvs,
                         eastl::vector<nau::math::float4>& tangents)
    {
        for (size_t i = 0; i + 2 < indexCount; i += 3)
        {
            const Index index[3] = {indices[i + 0], indices[i + 1], indices[i + 2]};

            const nau::math::Vector3 v0 = nau::math::Vector3(positions[index[0]].x, positions[index[0]].y, positions[index[0]].z);
            const nau::math::Vector3 v1 = nau::math::Vector3(positions[index[1]].x, positions[index[1]].y, positions[index[1]].z);
            const nau::math::Vector3 v2 = nau::math::Vector3(positions[index[2]].x, positions[index[2]].y, positions[index[2]].z);

            const nau::math::Vector2 uv0 = nau::math::Vector2(uvs[index[0]].x, uvs[index[0]].y);
            const nau::math::Vector2 uv1 = nau::math::Vector2(uvs[index[1]].x, uvs[index[1]].y);
            const nau::math::Vector2 uv2 = nau::math::Vector2(uvs[index[2]].x, uvs[index[2]].y);

            const nau::math::Vector3 dv1 = v1 - v0;
            const nau::math::Vector3 dv2 = v2 - v0;

            const nau::math::Vector2 duv1 = uv1 - uv0;
            const nau::math::Vector2 duv2 = uv2 - uv0;

            const float determinant = duv1[0] * duv2[1] - duv1[1] * duv2[0];
            if (std::abs(determinant) < 1e-12f)
            {
                continue;
            }

            const nau::math::Vector3 tangent = (dv1 * duv2[1] - dv2 * duv1[1]) * (1.0f / determinant);

            for (const Index vertex : index)
            {
                const nau::math::Vector3 normal = nau::math::Vector3(normals[vertex].x, normals[vertex].y, normals[vertex].z);
                const nau::math::Vector3 orthogonal = tangent - Vectormath::SSE::dot(tangent, normal) * normal;
                if (Vectormath::SSE::lengthSqr(orthogonal) > 1e-12f)
                {
                    const nau::math::Vector3 unit = Vectormath::SSE::normalize(orthogonal);
                    tangents[vertex] = nau::math::float4(unit[0], unit[1], unit[2], 1.0f);
                }
            }
        }
    }

    // A buffer in video memory: a vertex fetched from the upload heap goes over the bus (Thunderbolt for an external GPU) in
    // every pass. The data reaches it through the temporary upload memory of the driver.
    template <typename T>
    Sbuffer* createGeometryBuffer(const T* data, size_t count, Sbuffer* (*create)(int, int, const char8_t*), int flags, const char8_t* name)
    {
        const size_t size = count * sizeof(T);
        Sbuffer* buffer = create(static_cast<int>(size), flags, name);

        void* memory = nullptr;
        buffer->lock(0, static_cast<uint32_t>(size), &memory, VBLOCK_WRITEONLY);
        std::memcpy(memory, data, size);
        buffer->unlock();

        return buffer;
    }

    Sbuffer* createVertexBuffer(int size, int flags, const char8_t* name)
    {
        return d3d::create_vb(size, flags, name);
    }

    Sbuffer* createIndexBuffer(int size, int flags, const char8_t* name)
    {
        return d3d::create_ib(size, flags, name);
    }
}  // namespace

namespace
{
    // One level of detail: the CPU copies, the tangents and the buffers in video memory.
    void loadLod(nau::IMeshAssetAccessor& meshAccessor, nau::StaticMeshLod& lod)
    {
        using namespace nau;

        const auto meshDesc = meshAccessor.getDescription();

        lod.m_indexCount = meshDesc.indexCount;
        lod.m_vertexCount = meshDesc.vertexCount;
        lod.m_indices32 = meshDesc.indexFormat == ElementFormat::Uint32;

        const size_t indexBufferSize = meshDesc.indexCount * (lod.m_indices32 ? sizeof(uint32_t) : sizeof(uint16_t));
        lod.m_cpuIndices.resize(indexBufferSize);
        if (indexBufferSize != 0)
        {
            meshAccessor.copyIndices(lod.m_cpuIndices.data(), indexBufferSize, meshDesc.indexFormat).ignore();
        }

        lod.m_cpuPositions.resize(meshDesc.vertexCount);
        lod.m_cpuTexCoords.resize(meshDesc.vertexCount);
        eastl::vector<nau::math::float3> normals(meshDesc.vertexCount);
        eastl::vector<nau::math::float4> tangents(meshDesc.vertexCount, nau::math::float4(1.0f, 0.0f, 0.0f, 1.0f));

        if (meshDesc.vertexCount != 0)
        {
            eastl::array<OutputVertAttribDescription, 4> outLayout;

            auto& posDesc = outLayout[0];
            {
                posDesc.semantic = "POSITION";
                posDesc.semanticIndex = 0;
                posDesc.elementFormat = ElementFormat::Float;
                posDesc.attributeType = AttributeType::Vec3;
                posDesc.byteStride = 0;
                posDesc.outputBuffer = lod.m_cpuPositions.data();
                posDesc.outputBufferSize = meshDesc.vertexCount * sizeof(float[3]);
            }

            auto& nrmDesc = outLayout[1];
            {
                nrmDesc.semantic = "NORMAL";
                nrmDesc.semanticIndex = 0;
                nrmDesc.elementFormat = ElementFormat::Float;
                nrmDesc.attributeType = AttributeType::Vec3;
                nrmDesc.byteStride = 0;
                nrmDesc.outputBuffer = normals.data();
                nrmDesc.outputBufferSize = meshDesc.vertexCount * sizeof(float[3]);
            }

            auto& tangentDesc = outLayout[2];
            {
                tangentDesc.semantic = "TANGENT";
                tangentDesc.semanticIndex = 0;
                tangentDesc.elementFormat = ElementFormat::Float;
                tangentDesc.attributeType = AttributeType::Vec4;
                tangentDesc.byteStride = 0;
                tangentDesc.outputBuffer = tangents.data();
                tangentDesc.outputBufferSize = meshDesc.vertexCount * sizeof(float[4]);
            }

            auto& uv0Desc = outLayout[3];
            {
                uv0Desc.semantic = "TEXCOORD";
                uv0Desc.semanticIndex = 0;
                uv0Desc.elementFormat = ElementFormat::Float;
                uv0Desc.attributeType = AttributeType::Vec2;
                uv0Desc.byteStride = 0;
                uv0Desc.outputBuffer = lod.m_cpuTexCoords.data();
                uv0Desc.outputBufferSize = meshDesc.vertexCount * sizeof(float[2]);
            }

            meshAccessor.copyVertAttribs(outLayout).ignore();

            nau::math::AABB aabb = nau::math::AABB();
            aabb.InitFromVertsSlow(lod.m_cpuPositions.data(), meshDesc.vertexCount);
            lod.m_localBBox = nau::math::BBox3(aabb.minBounds, aabb.maxBounds);
        }

        if (meshDesc.indexCount != 0 && meshDesc.vertexCount != 0)
        {
            if (lod.m_indices32)
            {
                computeTangents(reinterpret_cast<const uint32_t*>(lod.m_cpuIndices.data()), meshDesc.indexCount, lod.m_cpuPositions, normals, lod.m_cpuTexCoords, tangents);
            }
            else
            {
                computeTangents(reinterpret_cast<const uint16_t*>(lod.m_cpuIndices.data()), meshDesc.indexCount, lod.m_cpuPositions, normals, lod.m_cpuTexCoords, tangents);
            }
        }

        // A vertex whose triangles have no usable texture coordinates keeps the default tangent. Along the normal it breaks
        // the tangent frame of the normal mapping (GetTBN: NaN, a black pixel); any perpendicular will do instead.
        for (uint32_t vertex = 0; vertex < meshDesc.vertexCount; ++vertex)
        {
            const nau::math::Vector3 normal(normals[vertex].x, normals[vertex].y, normals[vertex].z);
            const nau::math::Vector3 tangent(tangents[vertex].x, tangents[vertex].y, tangents[vertex].z);
            if (std::abs(float(Vectormath::SSE::dot(normal, tangent))) < 0.99f)
            {
                continue;
            }

            const nau::math::Vector3 axis = std::abs(normals[vertex].y) < 0.9f ? nau::math::Vector3(0.0f, 1.0f, 0.0f) : nau::math::Vector3(1.0f, 0.0f, 0.0f);
            const nau::math::Vector3 perpendicular = Vectormath::SSE::cross(axis, normal);
            if (float(Vectormath::SSE::lengthSqr(perpendicular)) > 1e-12f)
            {
                const nau::math::Vector3 unit = Vectormath::SSE::normalize(perpendicular);
                tangents[vertex] = nau::math::float4(unit[0], unit[1], unit[2], 1.0f);
            }
        }

        d3d::driver_command(DRV3D_COMMAND_ACQUIRE_OWNERSHIP, NULL, NULL, NULL);

        if (indexBufferSize != 0)
        {
            lod.m_indexBuffer = createGeometryBuffer(lod.m_cpuIndices.data(), indexBufferSize, &createIndexBuffer, lod.m_indices32 ? SBCF_INDEX32 : 0, u8"IndexBuf");
        }
        else
        {
            lod.m_indexBuffer = nullptr;
        }

        if (meshDesc.vertexCount != 0)
        {
            lod.m_positionsBuffer = createGeometryBuffer(lod.m_cpuPositions.data(), meshDesc.vertexCount, &createVertexBuffer, 0, u8"posBuf");
            lod.m_normalsBuffer = createGeometryBuffer(normals.data(), meshDesc.vertexCount, &createVertexBuffer, 0, u8"normBuf");
            lod.m_tangentsBuffer = createGeometryBuffer(tangents.data(), meshDesc.vertexCount, &createVertexBuffer, 0, u8"tangentBuf");
            lod.m_texCoordsBuffer = createGeometryBuffer(lod.m_cpuTexCoords.data(), meshDesc.vertexCount, &createVertexBuffer, 0, u8"texBuf");
        }
        else
        {
            lod.m_positionsBuffer = nullptr;
            lod.m_normalsBuffer = nullptr;
            lod.m_tangentsBuffer = nullptr;
            lod.m_texCoordsBuffer = nullptr;
        }

        d3d::driver_command(DRV3D_COMMAND_RELEASE_OWNERSHIP, NULL, NULL, NULL);
    }
}  // namespace

nau::async::Task<nau::Ptr<nau::StaticMesh>> nau::StaticMesh::createFromStaticMeshAccessor(IMeshAssetAccessor& meshAccessor)
{
    nau::StaticMesh::Ptr mesh = rtti::createInstance<StaticMesh>();

    // Level 0 is the mesh itself, the simplified levels come from the optional interface of the accessor.
    const eastl::vector<IMeshLodsAccessor::Lod> lodSources = meshAccessor.is<IMeshLodsAccessor>()
                                                                 ? meshAccessor.as<IMeshLodsAccessor&>().getLods()
                                                                 : eastl::vector<IMeshLodsAccessor::Lod>{};
    mesh->lods.reserve(1 + lodSources.size());

    loadLod(meshAccessor, mesh->lods.emplace_back());
    const StaticMeshLod& lod0 = mesh->lods.front();
    if (lod0.m_vertexCount != 0)
    {
        mesh->m_localBSphere = nau::math::BSphere3();
        mesh->m_localBSphere += lod0.m_localBBox;

        NAU_ASSERT(mesh->m_localBSphere.r > 0.00001f);
    }

    for (const IMeshLodsAccessor::Lod& source : lodSources)
    {
        if (!source.accessor || source.accessor->getDescription().indexCount == 0)
        {
            continue;
        }
        // A shadow-only level before an ordinary one would hide the ordinary one from the views that are seen.
        if (!source.shadowOnly && !mesh->m_lodShadowOnly.empty() && mesh->m_lodShadowOnly.back())
        {
            NAU_LOG_WARNING("A level of detail follows a shadow-only level: the level and the following ones are ignored");
            break;
        }
        loadLod(*source.accessor, mesh->lods.emplace_back());
        mesh->m_lodDistances.push_back(source.distance);
        mesh->m_lodShadowOnly.push_back(source.shadowOnly);
    }

    // load material: the default one in the only slot of every level (StaticMeshComponent overrides it)
    static MaterialAssetRef material {AssetPath{"file:/res/materials/embedded/standard_opaque.nmat_json"}};
    const ReloadableAssetView::Ptr defaultMaterial = co_await material.getReloadableAssetViewTyped<MaterialAssetView>();
    for (StaticMeshLod& lod : mesh->lods)
    {
        nau::MaterialSlot& slot = lod.m_materialSlots.emplace_back();
        slot.m_startIndex = 0;
        slot.m_endIndex = lod.m_indexCount;
        slot.m_material = defaultMaterial;
    }

    co_return mesh;
}

bool nau::StaticMesh::createFromGeneratedData()
{
    return false;
}

const nau::StaticMeshLod& nau::StaticMesh::getLod(uint32_t lodInd) const
{
    NAU_ASSERT(lodInd < lods.size());

    return lods[lodInd];
}

uint32_t nau::StaticMesh::getLodsCount() const
{
    return lods.size();
}

uint32_t nau::StaticMesh::selectLod(float distance, bool shadowLevels) const
{
    uint32_t lod = 0;
    while (lod < m_lodDistances.size() && distance > m_lodDistances[lod] && (shadowLevels || !m_lodShadowOnly[lod]))
    {
        ++lod;
    }
    return lod;
}
