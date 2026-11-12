// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <EASTL/optional.h>
#include <EASTL/span.h>
#include <EASTL/vector.h>

#include "nau/assets/asset_accessor.h"
#include "nau/async/task.h"
#include "nau/utils/result.h"

namespace nau
{
    enum class ElementFormat : uint8_t
    {
        Uint8 = NauFlag(1),
        Uint16 = NauFlag(2),
        Uint32 = NauFlag(3),
        Float = NauFlag(4),
    };

    NAU_DEFINE_TYPED_FLAG(ElementFormat)

    enum class AttributeType
    {
        Scalar,
        Vec2,
        Vec3,
        Vec4
    };

    struct VertAttribDescription
    {
        std::string semantic;
        unsigned semanticIndex;
        ElementFormat elementFormat;
        AttributeType attributeType;
    };

    struct OutputVertAttribDescription : VertAttribDescription
    {
        void* outputBuffer;
        size_t outputBufferSize;
        size_t byteStride;
    };

    struct MeshDescription
    {
        unsigned indexCount;
        unsigned vertexCount;
        ElementFormat indexFormat;
    };

    struct NAU_ABSTRACT_TYPE IMeshAssetAccessor : IAssetAccessor
    {
        NAU_INTERFACE(nau::IMeshAssetAccessor, IAssetAccessor)

        virtual ElementFormatFlag getSupportedIndexTypes() const = 0;

        virtual MeshDescription getDescription() const = 0;

        virtual eastl::vector<VertAttribDescription> getVertAttribDescriptions() const = 0;

        virtual Result<> copyVertAttribs(eastl::span<OutputVertAttribDescription>) const = 0;

        virtual Result<> copyIndices(void* outputBuffer, size_t outputBufferSize, ElementFormat outputIndexFormat) const = 0;
    };

    /**
     * @brief Optional interface of a mesh accessor: simplified versions of the mesh (levels of detail).
     *
     * The levels follow the mesh itself (level 0), each one coarser than the previous. A level replaces the previous one
     * when the viewer is farther than its distance from the origin of an instance; the distance is in the units of the mesh,
     * that is, it grows with the scale of the instance. The last levels can be shadow-only: drawn by the views that ask for
     * them (the shadow cascades), the other views stay at the last level before them.
     */
    struct NAU_ABSTRACT_TYPE IMeshLodsAccessor : IRefCounted
    {
        NAU_INTERFACE(nau::IMeshLodsAccessor, IRefCounted)

        struct Lod
        {
            nau::Ptr<IMeshAssetAccessor> accessor;
            float distance = 0.0f;
            bool shadowOnly = false;
        };

        virtual eastl::vector<Lod> getLods() const = 0;
    };

}  // namespace nau
