// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./gltf_mesh_accessor.h"

namespace nau
{
    namespace
    {
        inline nau::ElementFormat gltf2ElementFormat(unsigned gltfDataType)
        {
            // constexpr unsigned Signed8 = 5120;
            constexpr unsigned Unsigned8 = 5121;
            // constexpr unsigned Signed16 = 5122;
            constexpr unsigned Unsigned16 = 5123;
            constexpr unsigned Unsigned32 = 5125;
            constexpr unsigned Float32 = 5126;

            if(gltfDataType == Unsigned8)
            {
                return ElementFormat::Uint8;
            }
            else if(gltfDataType == Unsigned16)
            {
                return ElementFormat::Uint16;
            }
            else if(gltfDataType == Unsigned32)
            {
                return ElementFormat::Uint32;
            }
            else if(gltfDataType == Float32)
            {
                return ElementFormat::Float;
            }
            NAU_ASSERT(false, "Unknown element format");

            return ElementFormat::Float;
        }

        inline nau::AttributeType gltf2AttributeType(eastl::string_view type)
        {
            if(strings::icaseEqual(type, "SCALAR"))
            {
                return AttributeType::Scalar;
            }
            else if(strings::icaseEqual(type, "VEC2"))
            {
                return AttributeType::Vec2;
            }
            else if(strings::icaseEqual(type, "VEC3"))
            {
                return AttributeType::Vec3;
            }
            else if(strings::icaseEqual(type, "VEC4"))
            {
                return AttributeType::Vec4;
            }

            NAU_ASSERT("Unknown attribute type ({})", type);
            return AttributeType::Scalar;
        }

        inline size_t formatByteSize(ElementFormat format)
        {
            if(format == ElementFormat::Uint8)
            {
                return sizeof(uint8_t);
            }
            else if(format == ElementFormat::Uint16)
            {
                return sizeof(uint16_t);
            }

            NAU_ASSERT(format == ElementFormat::Uint32 || format == ElementFormat::Float);
            NAU_ASSERT(sizeof(float) == sizeof(uint32_t));

            return sizeof(uint32_t);
        }

        inline size_t attributeComponentsCount(AttributeType type)
        {
            if(type == AttributeType::Scalar)
            {
                return 1;
            }
            else if(type == AttributeType::Vec2)
            {
                return 2;
            }
            else if(type == AttributeType::Vec3)
            {
                return 3;
            }

            NAU_ASSERT(type == AttributeType::Vec4);

            return 4;
        }

        eastl::optional<double> numberOf(const RuntimeValue::Ptr& value)
        {
            if (!value)
            {
                return eastl::nullopt;
            }
            if (auto* const floatValue = value->as<RuntimeFloatValue*>())
            {
                return floatValue->getDouble();
            }
            if (auto* const integerValue = value->as<RuntimeIntegerValue*>())
            {
                return static_cast<double>(integerValue->getInt64());
            }
            return eastl::nullopt;
        }

        // An array of numbers in the extras of a glTF object; empty if there is no such key.
        eastl::vector<double> numbersOf(const eastl::map<eastl::string, RuntimeValue::Ptr>& extras, const char* key)
        {
            eastl::vector<double> numbers;
            const auto found = extras.find(key);
            if (found == extras.end() || !found->second)
            {
                return numbers;
            }

            auto* const collection = found->second->as<RuntimeReadonlyCollection*>();
            if (!collection)
            {
                return numbers;
            }

            for (size_t i = 0; i < collection->getSize(); ++i)
            {
                if (const eastl::optional<double> number = numberOf(collection->getAt(i)))
                {
                    numbers.push_back(*number);
                }
            }
            return numbers;
        }

    }  // namespace

    GltfMeshAssetAccessor::GltfMeshAssetAccessor(const GltfFile& file, size_t meshIndex, const eastl::vector<io::IFile::Ptr>& bufferFiles, bool withLods)
    {
        NAU_ASSERT(meshIndex < file.meshes.size());

        const auto& mesh = file.meshes[meshIndex];
        if(mesh.primitives.empty())
        {
            return;
        }

        if (withLods)
        {
            // The meshes of the levels are read without levels of their own: no chains, no cycles.
            const eastl::vector<double> lodMeshes = numbersOf(mesh.extras, "nau_lod_meshes");
            const eastl::vector<double> lodDistances = numbersOf(mesh.extras, "nau_lod_distances");
            NAU_ASSERT(lodMeshes.size() == lodDistances.size(), "Mesh ({}): nau_lod_meshes and nau_lod_distances differ in size", mesh.name);

            const size_t count = eastl::min(lodMeshes.size(), lodDistances.size());
            m_lods.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t lodMeshIndex = static_cast<size_t>(lodMeshes[i]);
                if (lodMeshIndex >= file.meshes.size() || lodMeshIndex == meshIndex || file.meshes[lodMeshIndex].primitives.empty())
                {
                    NAU_ASSERT(false, "Mesh ({}): invalid level of detail mesh {}", mesh.name, lodMeshIndex);
                    continue;
                }

                Lod& lod = m_lods.emplace_back();
                lod.accessor = rtti::createInstance<GltfMeshAssetAccessor, IMeshAssetAccessor>(file, lodMeshIndex, bufferFiles, false);
                lod.distance = static_cast<float>(lodDistances[i]);
            }
        }

        const auto& subMesh = mesh.primitives.front();

        const auto& posAccessor = file.accessors[subMesh.attributes.at("POSITION")];
        const auto& indexAccessor = file.accessors[subMesh.indices];
        const auto& indexBufferView = file.bufferViews[indexAccessor.bufferView];

        m_meshDescription.indexCount = indexAccessor.count;
        m_meshDescription.vertexCount = posAccessor.count;
        m_meshDescription.indexFormat = gltf2ElementFormat(indexAccessor.componentType);
        NAU_ASSERT(gltf2AttributeType(indexAccessor.type) == AttributeType::Scalar);
        NAU_ASSERT(m_meshDescription.indexFormat == ElementFormat::Uint16 || m_meshDescription.indexFormat == ElementFormat::Uint32);

        m_vertAttributes.reserve(subMesh.attributes.size());
        m_binaryAccessors.reserve(subMesh.attributes.size() + 1);  // +1 - indices

        auto& binaryAccessor = m_binaryAccessors.emplace_back();

        binaryAccessor.file = bufferFiles[indexBufferView.buffer];
        binaryAccessor.attrib = nullptr;
        binaryAccessor.offset = indexBufferView.byteOffset;
        binaryAccessor.size = indexBufferView.byteLength;

        for(const auto& [attribName, accessorIndex] : subMesh.attributes)
        {
            const auto& accessor = file.accessors[accessorIndex];
            const auto& bufferView = file.bufferViews[accessor.bufferView];
            NAU_ASSERT(!bufferView.byteStride, "STRIDE IS NOT SUPPORTED");

            VertAttribDescription& vertAttribDescription = m_vertAttributes.emplace_back();

            if(const auto [semanticName, semanticIndex] = strings::cut(attribName, '_'); !semanticName.empty())
            {
                vertAttribDescription.semantic = std::string{semanticName.data(), semanticName.size()};
                vertAttribDescription.semanticIndex = strings::lexicalCast<unsigned>(semanticIndex);
            }
            else
            {
                vertAttribDescription.semantic = std::string{attribName.data(), attribName.size()};
                vertAttribDescription.semanticIndex = 0;
            }

            vertAttribDescription.attributeType = gltf2AttributeType(accessor.type);
            vertAttribDescription.elementFormat = gltf2ElementFormat(accessor.componentType);
            
            NAU_ASSERT(bufferView.byteLength == attributeComponentsCount(vertAttribDescription.attributeType) * formatByteSize(vertAttribDescription.elementFormat) * accessor.count);

            auto& binaryAccessor = m_binaryAccessors.emplace_back();
            binaryAccessor.file = bufferFiles[bufferView.buffer];
            binaryAccessor.attrib = &vertAttribDescription;
            binaryAccessor.offset = bufferView.byteOffset;
            binaryAccessor.size = bufferView.byteLength;
        }
    }

    eastl::vector<IMeshLodsAccessor::Lod> GltfMeshAssetAccessor::getLods() const
    {
        return m_lods;
    }

    ElementFormatFlag GltfMeshAssetAccessor::getSupportedIndexTypes() const
    {
        return ElementFormat::Uint16 | ElementFormat::Uint32;
    }

    MeshDescription GltfMeshAssetAccessor::getDescription() const
    {
        return m_meshDescription;
    }

    eastl::vector<VertAttribDescription> GltfMeshAssetAccessor::getVertAttribDescriptions() const
    {
        return m_vertAttributes;
    }

    Result<> GltfMeshAssetAccessor::copyVertAttribs(eastl::span<OutputVertAttribDescription> outputLayout) const
    {
        const auto findAccessor = [&](const VertAttribDescription& desc) -> const BinaryAccessor*
        {
            // + 1 - skip indices
            auto iter = eastl::find_if(m_binaryAccessors.begin() + 1, m_binaryAccessors.end(), [&desc](const BinaryAccessor& ba)
            {
                return ba.attrib && (ba.attrib->semantic == desc.semantic && ba.attrib->semanticIndex == desc.semanticIndex);
            });

            return iter != m_binaryAccessors.end() ? &(*iter) : nullptr;
        };

        for(const auto& outputDesc : outputLayout)
        {
            auto const binAccessor = findAccessor(outputDesc);
            if (!binAccessor)
            {
                continue;
            }

            NAU_ASSERT(outputDesc.byteStride == 0);
            NAU_ASSERT(outputDesc.attributeType == binAccessor->attrib->attributeType);

            auto stream = binAccessor->file->createStream(io::AccessMode::Read);
            stream->setPosition(io::OffsetOrigin::Begin, binAccessor->offset);

            auto* reader = stream->as<io::IStreamReader*>();
            NAU_ASSERT(reader);

            if (binAccessor->attrib->elementFormat == outputDesc.elementFormat)
            {
                NAU_ASSERT(outputDesc.outputBufferSize == binAccessor->size);

                io::copyFromStream(outputDesc.outputBuffer, outputDesc.outputBufferSize, *reader).ignore();
            }
            else
            {
                NAU_ASSERT(outputDesc.elementFormat == ElementFormat::Uint32);
                uint32_t* buf = reinterpret_cast<uint32_t*>(outputDesc.outputBuffer);
                
                const size_t elementCount = binAccessor->size / formatByteSize(binAccessor->attrib->elementFormat);
                NAU_ASSERT(outputDesc.outputBufferSize == elementCount * 4);

                if (binAccessor->attrib->elementFormat == ElementFormat::Uint8)
                {
                    std::vector<uint8_t> tmpU8;
                    tmpU8.resize(elementCount);

                    io::copyFromStream(tmpU8.data(), binAccessor->size, *reader).ignore();

                    for (size_t i = 0; i < elementCount; ++i)
                    {
                        buf[i] = static_cast<uint32_t>(tmpU8[i]);
                    }
                }
                else if (binAccessor->attrib->elementFormat == ElementFormat::Uint16)
                {
                    NAU_ASSERT(outputDesc.outputBufferSize == binAccessor->size * 2);

                    std::vector<uint16_t> tmpU16;
                    tmpU16.resize(elementCount);

                    io::copyFromStream(tmpU16.data(), binAccessor->size, *reader).ignore();

                    for (size_t i = 0; i < elementCount; ++i)
                    {
                        buf[i] = static_cast<uint32_t>(tmpU16[i]);
                    }
                }
            }
        }

        return ResultSuccess;
    }

    Result<> GltfMeshAssetAccessor::copyIndices(void* outputBuffer, size_t outputBufferSize, ElementFormat outputIndexFormat) const
    {
        const ElementFormatFlag supportedIndexFormats = getSupportedIndexTypes();
        NAU_ASSERT(supportedIndexFormats.has(m_meshDescription.indexFormat));

        auto& binaryAccessor = m_binaryAccessors.front();

        auto stream = binaryAccessor.file->createStream(io::AccessMode::Read);
        stream->setPosition(io::OffsetOrigin::Begin, binaryAccessor.offset);

        auto* reader = stream->as<io::IStreamReader*>();
        NAU_ASSERT(reader);

        NAU_ASSERT(outputIndexFormat == ElementFormat::Uint16 || outputIndexFormat == m_meshDescription.indexFormat);

        const size_t expectedBufferLength = formatByteSize(m_meshDescription.indexFormat) * m_meshDescription.indexCount;

        NAU_ASSERT(binaryAccessor.size <= expectedBufferLength);

        if (m_meshDescription.indexFormat == outputIndexFormat)
        {
            io::copyFromStream(outputBuffer, outputBufferSize, *reader).ignore();
        }
        else if (m_meshDescription.indexFormat == ElementFormat::Uint32)
        {
            std::vector<uint32_t> tmpU32;
            tmpU32.resize(binaryAccessor.size);

            io::copyFromStream(tmpU32.data(), binaryAccessor.size, *reader).ignore();

            uint16_t* buf = reinterpret_cast<uint16_t*>(outputBuffer);
            for (size_t i = 0; i < tmpU32.size(); ++i)
            {
                buf[i] = static_cast<uint16_t>(tmpU32[i]);
            }
        }
        else
        {
            NAU_ASSERT(false);
        }

        return ResultSuccess;
    }
}  // namespace nau
