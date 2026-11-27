
#include "nau/mgpu/bridge/mgpu_shadows.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Utils/d3dx12.h"
#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Graphics/GCommandQueue.h"
#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GResource.h"
#include "nau/mgpu/Graphics/GRootSignature.h"
#include "nau/mgpu/Graphics/GTexture.h"
#include "nau/mgpu/Graphics/GraphicPSO.h"
#include "nau/mgpu/Common/GCrossAdapterResource.h"

#include "bridge_common.h"

namespace nau::mgpu::shadows
{
    namespace gfx = nau::PEPEngine::Graphics;
    using bridge::detail::SourceShader;

    namespace
    {

        constexpr uint32_t VIEW_PROJ_CONSTANTS = 16;
        constexpr uint32_t DRAW_CONSTANTS = 2;
        constexpr UINT CONSTANTS_PARAMETER = 0;
        constexpr UINT MATRICES_PARAMETER = 1;
        constexpr UINT MASK_PARAMETER = 2;
        constexpr UINT MAX_MASKS = 1024;

        constexpr const char* SHADOW_SHADER_SOURCE = R"(
cbuffer DrawParams : register(b0)
{
    float4x4 vp;
    uint matrixOffset;
    uint boneCount;
};

StructuredBuffer<float4x4> matrices : register(t0);

float4 ShadowStaticVS(float3 position : POSITION, uint instance : SV_InstanceID) : SV_Position
{
    return mul(mul(vp, matrices[matrixOffset + instance]), float4(position, 1.0));
}

float4 ShadowSkinnedVS(float3 position : POSITION, float4 boneWeights : BLENDWEIGHT,
                       uint4 boneIndices : BLENDINDICES) : SV_Position
{
    float4 skinned = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const uint bone = boneIndices[i];
        if (bone < boneCount)
        {
            skinned += boneWeights[i] * mul(matrices[matrixOffset + bone], float4(position, 1.0));
        }
    }
    return mul(vp, skinned);
}

struct MaskedVertex
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

Texture2D maskTexture : register(t1);
SamplerState maskSampler : register(s0);

MaskedVertex ShadowMaskedVS(float3 position : POSITION, float2 texCoord : TEXCOORD0, uint instance : SV_InstanceID)
{
    MaskedVertex output;
    output.position = mul(mul(vp, matrices[matrixOffset + instance]), float4(position, 1.0));
    output.texCoord = float2(texCoord.x, 1.0 - texCoord.y);
    return output;
}

void ShadowMaskedPS(MaskedVertex input)
{
    clip(maskTexture.Sample(maskSampler, input.texCoord).a - 0.5);
}
)";

        DXGI_FORMAT typelessFormat(const AtlasFormat format)
        {
            return format == AtlasFormat::Depth16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
        }

        DXGI_FORMAT depthFormat(const AtlasFormat format)
        {
            return format == AtlasFormat::Depth16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT;
        }

        uint32_t bytesPerTexel(const AtlasFormat format)
        {
            return format == AtlasFormat::Depth16 ? 2u : 4u;
        }

        std::shared_ptr<gfx::GResource> createUploadBuffer(const std::shared_ptr<gfx::GDevice>& device, const size_t size, const std::wstring& name)
        {
            return std::make_shared<gfx::GResource>(device, CD3DX12_RESOURCE_DESC::Buffer(std::max<size_t>(size, 256)),
                                                    name, nullptr, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD));
        }

        void writeUploadBuffer(gfx::GResource& buffer, const void* data, const size_t size)
        {
            void* mapped = nullptr;
            const D3D12_RANGE noRead = {0, 0};
            ThrowIfFailed(buffer.GetD3D12Resource()->Map(0, &noRead, &mapped));
            std::memcpy(mapped, data, size);
            buffer.GetD3D12Resource()->Unmap(0, nullptr);
        }
    }  // namespace

    struct CascadeRenderer::Impl
    {
        using Clock = std::chrono::steady_clock;

        struct Mesh
        {
            std::shared_ptr<gfx::GResource> positions;
            std::shared_ptr<gfx::GResource> boneWeights;
            std::shared_ptr<gfx::GResource> boneIndices;
            std::shared_ptr<gfx::GResource> indices;
            std::shared_ptr<gfx::GResource> texCoords;
            std::array<D3D12_VERTEX_BUFFER_VIEW, 3> vertexViews = {};
            std::array<D3D12_VERTEX_BUFFER_VIEW, 2> maskedViews = {};
            D3D12_INDEX_BUFFER_VIEW indexView = {};
            bool skinned = false;
        };

        enum class SlotState
        {
            Free,
            Rendering,
            Drawn,
            Transferring,
            Ready,
            Handed
        };

        struct Slot
        {
            SlotState state = SlotState::Free;
            std::shared_ptr<GCrossAdapterResource> shared;
            std::shared_ptr<gfx::GResource> local;
            uint32_t width = 0;
            uint32_t height = 0;
            AtlasFormat format = AtlasFormat::Depth16;
            std::shared_ptr<gfx::GResource> matrices;
            size_t matricesCapacity = 0;
            uint64_t fenceValue = 0;
            uint64_t transferFenceValue = 0;
            uint64_t sequence = 0;
            uint64_t userToken = 0;
            Clock::time_point submitted;
            Clock::time_point drawn;
            float latencyMs = 0;
        };

        struct Mask
        {
            ComPtr<ID3D12Resource> texture;
            D3D12_GPU_DESCRIPTOR_HANDLE descriptor = {};
        };

        using PsoKey = std::tuple<bool, bool, AtlasFormat, int32_t, float, bool, bool>;

        bool ready = false;
        std::string error;
        std::string primaryName;
        std::string secondaryName;

        std::shared_ptr<gfx::GDevice> primaryDevice;
        std::shared_ptr<gfx::GDevice> secondaryDevice;

        std::shared_ptr<gfx::GTexture> atlas;
        gfx::GDescriptor atlasDsv;
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        AtlasFormat atlasFormat = AtlasFormat::Depth16;

        std::shared_ptr<gfx::GRootSignature> signature;
        std::shared_ptr<SourceShader> staticVS;
        std::shared_ptr<SourceShader> skinnedVS;
        std::shared_ptr<SourceShader> maskedVS;
        std::shared_ptr<SourceShader> maskedPS;
        ComPtr<ID3D12DescriptorHeap> maskHeap;
        UINT maskDescriptorSize = 0;
        UINT maskCount = 0;
        std::unordered_map<uint64_t, Mask> masks;
        std::map<PsoKey, std::shared_ptr<gfx::GraphicPSO>> psos;

        std::unordered_map<uint64_t, Mesh> meshes;
        std::array<Slot, SLOT_COUNT> slots;

        bool geometryInVideoMemory = false;
        std::shared_ptr<gfx::GCommandList> uploadList;
        std::vector<std::shared_ptr<gfx::GResource>> uploadBuffers;
        struct UploadBatch
        {
            uint64_t fenceValue = 0;
            std::vector<std::shared_ptr<gfx::GResource>> buffers;
        };
        std::vector<UploadBatch> uploadsInFlight;
        std::vector<uint32_t> drawOrder;
        std::vector<D3D12_RECT> clearRects;
        uint64_t nextSequence = 0;
        RendererStats stats;

        static constexpr UINT DRAW_STAMPS = 3;
        static constexpr UINT TRANSFER_STAMPS = 2;
        ComPtr<ID3D12QueryHeap> drawQueries;
        ComPtr<ID3D12Resource> drawReadback;
        UINT64* drawStamps = nullptr;
        double drawTicksPerMs = 0;
        ComPtr<ID3D12QueryHeap> transferQueries;
        ComPtr<ID3D12Resource> transferReadback;
        UINT64* transferStamps = nullptr;
        double transferTicksPerMs = 0;

        std::shared_ptr<gfx::GCommandQueue>& graphicsQueue2() const
        {
            return secondaryDevice->GetCommandQueue(gfx::GQueueType::Graphics);
        }

        std::shared_ptr<gfx::GCommandQueue>& copyQueue1() const
        {
            return primaryDevice->GetCommandQueue(gfx::GQueueType::Copy);
        }

        bool initialize(const char* nauAdapterName)
        {
            try
            {
                if (!bridge::detail::selectDevices(nauAdapterName, primaryDevice, secondaryDevice, error))
                {
                    return false;
                }
                primaryName = bridge::detail::toUtf8(primaryDevice->GetName());
                secondaryName = bridge::detail::toUtf8(secondaryDevice->GetName());

                D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
                const bool uma = FAILED(secondaryDevice->GetDXDevice()->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                                                                            sizeof(architecture))) ||
                                 architecture.UMA != FALSE;
                const char* const forceVideoMemory = std::getenv("NAU_MGPU_GEOMETRY_IN_VIDEO_MEMORY");
                geometryInVideoMemory = !uma || (forceVideoMemory != nullptr && std::strcmp(forceVideoMemory, "1") == 0);

                signature = std::make_shared<gfx::GRootSignature>();
                signature->AddConstantParameter(VIEW_PROJ_CONSTANTS + DRAW_CONSTANTS, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
                signature->AddShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
                const CD3DX12_DESCRIPTOR_RANGE maskRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
                signature->AddDescriptorParameter(&maskRange, 1, D3D12_SHADER_VISIBILITY_PIXEL);
                signature->AddStaticSampler(CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR));
                signature->Initialize(secondaryDevice);

                staticVS = std::make_shared<SourceShader>(SHADOW_SHADER_SOURCE, gfx::VertexShader, "ShadowStaticVS", "vs_5_1");
                staticVS->LoadAndCompile();
                skinnedVS = std::make_shared<SourceShader>(SHADOW_SHADER_SOURCE, gfx::VertexShader, "ShadowSkinnedVS", "vs_5_1");
                skinnedVS->LoadAndCompile();
                maskedVS = std::make_shared<SourceShader>(SHADOW_SHADER_SOURCE, gfx::VertexShader, "ShadowMaskedVS", "vs_5_1");
                maskedVS->LoadAndCompile();
                maskedPS = std::make_shared<SourceShader>(SHADOW_SHADER_SOURCE, gfx::PixelShader, "ShadowMaskedPS", "ps_5_1");
                maskedPS->LoadAndCompile();

                D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
                heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                heapDesc.NumDescriptors = MAX_MASKS;
                heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                ThrowIfFailed(secondaryDevice->GetDXDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&maskHeap)));
                maskDescriptorSize = secondaryDevice->GetDXDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                stats = {};
                initializeTimings();
                ready = true;
                error.clear();
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during initialize: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during initialize (see the D3D12 debug output)";
            }

            release();
            return false;
        }

        static ComPtr<ID3D12Resource> createReadback(ID3D12Device* device, const UINT64 size)
        {
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
            ComPtr<ID3D12Resource> buffer;
            ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&buffer)));
            return buffer;
        }

        void releaseTimings()
        {
            if (drawReadback && drawStamps)
            {
                drawReadback->Unmap(0, nullptr);
            }
            if (transferReadback && transferStamps)
            {
                transferReadback->Unmap(0, nullptr);
            }
            drawStamps = nullptr;
            transferStamps = nullptr;
            drawReadback.Reset();
            transferReadback.Reset();
            drawQueries.Reset();
            transferQueries.Reset();
            drawTicksPerMs = 0;
            transferTicksPerMs = 0;
        }

        void initializeTimings()
        {
            try
            {
                ID3D12Device* const device = secondaryDevice->GetDXDevice().Get();
                D3D12_QUERY_HEAP_DESC desc = {};
                desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                desc.Count = DRAW_STAMPS * SLOT_COUNT;
                ThrowIfFailed(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&drawQueries)));
                drawReadback = createReadback(device, desc.Count * sizeof(UINT64));
                ThrowIfFailed(drawReadback->Map(0, nullptr, reinterpret_cast<void**>(&drawStamps)));
                UINT64 frequency = 0;
                ThrowIfFailed(graphicsQueue2()->GetD3D12CommandQueue()->GetTimestampFrequency(&frequency));
                drawTicksPerMs = static_cast<double>(frequency) / 1000.0;
            }
            catch (...)
            {
                drawStamps = nullptr;
                drawReadback.Reset();
                drawQueries.Reset();
                drawTicksPerMs = 0;
            }

            try
            {
                ID3D12Device* const device = primaryDevice->GetDXDevice().Get();
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 options = {};
                if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) &&
                    options.CopyQueueTimestampQueriesSupported)
                {
                    D3D12_QUERY_HEAP_DESC desc = {};
                    desc.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
                    desc.Count = TRANSFER_STAMPS * SLOT_COUNT;
                    ThrowIfFailed(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&transferQueries)));
                    transferReadback = createReadback(device, desc.Count * sizeof(UINT64));
                    ThrowIfFailed(transferReadback->Map(0, nullptr, reinterpret_cast<void**>(&transferStamps)));
                    UINT64 frequency = 0;
                    ThrowIfFailed(copyQueue1()->GetD3D12CommandQueue()->GetTimestampFrequency(&frequency));
                    transferTicksPerMs = static_cast<double>(frequency) / 1000.0;
                }
            }
            catch (...)
            {
                transferStamps = nullptr;
                transferReadback.Reset();
                transferQueries.Reset();
                transferTicksPerMs = 0;
            }
        }

        UINT slotIndex(const Slot& slot) const
        {
            return static_cast<UINT>(&slot - slots.data());
        }

        void readDrawTimings(const Slot& slot)
        {
            if (drawStamps == nullptr || drawTicksPerMs <= 0)
            {
                return;
            }
            const UINT64* const stamps = drawStamps + slotIndex(slot) * DRAW_STAMPS;
            if (stamps[1] < stamps[0] || stamps[2] < stamps[1])
            {
                return;
            }
            stats.drawGpuMsTotal += static_cast<double>(stamps[1] - stamps[0]) / drawTicksPerMs;
            stats.sharedCopyGpuMsTotal += static_cast<double>(stamps[2] - stamps[1]) / drawTicksPerMs;
            ++stats.drawGpuSamples;
        }

        void readTransferTimings(const Slot& slot)
        {
            if (transferStamps == nullptr || transferTicksPerMs <= 0)
            {
                return;
            }
            const UINT64* const stamps = transferStamps + slotIndex(slot) * TRANSFER_STAMPS;
            if (stamps[1] < stamps[0])
            {
                return;
            }
            stats.transferGpuMsTotal += static_cast<double>(stamps[1] - stamps[0]) / transferTicksPerMs;
            ++stats.transferGpuSamples;
        }

        gfx::GraphicPSO& getPso(const bool skinned, const bool masked, const AtlasFormat format, const int32_t depthBias, const float slopeBias, const bool depthClip, const bool flipCull)
        {
            const PsoKey key{skinned, masked, format, depthBias, slopeBias, depthClip, flipCull};
            if (auto found = psos.find(key); found != psos.end())
            {
                return *found->second;
            }

            static const D3D12_INPUT_ELEMENT_DESC staticLayout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            static const D3D12_INPUT_ELEMENT_DESC skinnedLayout[] = {
                {    "POSITION", 0,    DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"BLENDINDICES", 0,  DXGI_FORMAT_R32G32B32A32_UINT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = signature->GetNativeSignature().Get();
            static const D3D12_INPUT_ELEMENT_DESC maskedLayout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0,    DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            desc.InputLayout = masked    ? D3D12_INPUT_LAYOUT_DESC{maskedLayout, _countof(maskedLayout)}
                               : skinned ? D3D12_INPUT_LAYOUT_DESC{skinnedLayout, _countof(skinnedLayout)}
                                         : D3D12_INPUT_LAYOUT_DESC{staticLayout, _countof(staticLayout)};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.RasterizerState.CullMode = masked ? D3D12_CULL_MODE_NONE : skinned ? D3D12_CULL_MODE_BACK
                                                                                    : D3D12_CULL_MODE_FRONT;
            if (flipCull && !masked)
            {
                desc.RasterizerState.CullMode = skinned ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
            }
            desc.RasterizerState.FrontCounterClockwise = TRUE;
            desc.RasterizerState.DepthBias = depthBias;
            desc.RasterizerState.DepthBiasClamp = 0.0f;
            desc.RasterizerState.SlopeScaledDepthBias = slopeBias;
            desc.RasterizerState.DepthClipEnable = depthClip ? TRUE : FALSE;
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 0;
            desc.DSVFormat = depthFormat(format);
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;

            auto pso = std::make_shared<gfx::GraphicPSO>(*signature);
            pso->SetPsoDesc(desc);
            pso->SetShader(masked ? maskedVS.get() : skinned ? skinnedVS.get()
                                                             : staticVS.get());
            if (masked)
            {
                pso->SetShader(maskedPS.get());
            }
            pso->Initialize(secondaryDevice);
            return *psos.emplace(key, std::move(pso)).first->second;
        }

        std::shared_ptr<gfx::GResource> createGeometryBuffer(const void* data, const size_t size, const std::wstring& name)
        {
            std::shared_ptr<gfx::GResource> upload = createUploadBuffer(secondaryDevice, size, geometryInVideoMemory ? name + L" Upload" : name);
            writeUploadBuffer(*upload, data, size);
            if (!geometryInVideoMemory)
            {
                return upload;
            }

            auto buffer = std::make_shared<gfx::GResource>(secondaryDevice, CD3DX12_RESOURCE_DESC::Buffer(std::max<size_t>(size, 256)),
                                                           name, nullptr, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!uploadList)
            {
                uploadList = graphicsQueue2()->GetCommandList();
            }
            auto* const raw = uploadList->GetGraphicsCommandList().Get();
            raw->CopyBufferRegion(buffer->GetD3D12Resource().Get(), 0, upload->GetD3D12Resource().Get(), 0, size);
            const auto toGeometry = CD3DX12_RESOURCE_BARRIER::Transition(buffer->GetD3D12Resource().Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                                                             D3D12_RESOURCE_STATE_INDEX_BUFFER);
            raw->ResourceBarrier(1, &toGeometry);
            uploadBuffers.push_back(upload);
            uploadBuffers.push_back(buffer);
            return buffer;
        }

        void flushUploads()
        {
            if (!uploadList)
            {
                return;
            }
            UploadBatch& batch = uploadsInFlight.emplace_back();
            batch.fenceValue = graphicsQueue2()->ExecuteCommandList(uploadList);
            batch.buffers = std::move(uploadBuffers);
            uploadBuffers.clear();
            uploadList.reset();
        }

        void releaseFinishedUploads()
        {
            const auto& queue = graphicsQueue2();
            uploadsInFlight.erase(std::remove_if(uploadsInFlight.begin(), uploadsInFlight.end(), [&queue](const UploadBatch& batch)
            {
                return queue->IsFinish(batch.fenceValue);
            }),
                                  uploadsInFlight.end());
        }

        bool registerMesh(const uint64_t key, const MeshDesc& desc)
        {
            if (!ready || desc.positions == nullptr || desc.indices == nullptr || desc.vertexCount == 0 ||
                desc.indexCount == 0)
            {
                return false;
            }

            try
            {
                const std::wstring name = L"MgpuShadows Mesh " + std::to_wstring(key);
                Mesh mesh;
                mesh.skinned = desc.boneWeights != nullptr && desc.boneIndices != nullptr;

                const size_t positionsSize = static_cast<size_t>(desc.vertexCount) * 3 * sizeof(float);
                mesh.positions = createGeometryBuffer(desc.positions, positionsSize, name + L" Positions");
                mesh.vertexViews[0] = {mesh.positions->GetD3D12Resource()->GetGPUVirtualAddress(),
                                       static_cast<UINT>(positionsSize), 3 * sizeof(float)};

                if (mesh.skinned)
                {
                    const size_t weightsSize = static_cast<size_t>(desc.vertexCount) * 4 * sizeof(float);
                    mesh.boneWeights = createGeometryBuffer(desc.boneWeights, weightsSize, name + L" Bone Weights");
                    mesh.vertexViews[1] = {mesh.boneWeights->GetD3D12Resource()->GetGPUVirtualAddress(),
                                           static_cast<UINT>(weightsSize), 4 * sizeof(float)};

                    const size_t indicesSize = static_cast<size_t>(desc.vertexCount) * 4 * sizeof(uint32_t);
                    mesh.boneIndices = createGeometryBuffer(desc.boneIndices, indicesSize, name + L" Bone Indices");
                    mesh.vertexViews[2] = {mesh.boneIndices->GetD3D12Resource()->GetGPUVirtualAddress(),
                                           static_cast<UINT>(indicesSize), 4 * sizeof(uint32_t)};
                }

                mesh.maskedViews[0] = mesh.vertexViews[0];
                if (desc.texCoords != nullptr)
                {
                    const size_t texCoordsSize = static_cast<size_t>(desc.vertexCount) * 2 * sizeof(float);
                    mesh.texCoords = createGeometryBuffer(desc.texCoords, texCoordsSize, name + L" Texture Coordinates");
                    mesh.maskedViews[1] = {mesh.texCoords->GetD3D12Resource()->GetGPUVirtualAddress(),
                                           static_cast<UINT>(texCoordsSize), 2 * sizeof(float)};
                }

                const size_t indexSize = desc.indices32 ? sizeof(uint32_t) : sizeof(uint16_t);
                const size_t indicesSize = static_cast<size_t>(desc.indexCount) * indexSize;
                mesh.indices = createGeometryBuffer(desc.indices, indicesSize, name + L" Indices");
                mesh.indexView = {mesh.indices->GetD3D12Resource()->GetGPUVirtualAddress(), static_cast<UINT>(indicesSize),
                                  desc.indices32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};

                if (meshes.count(key) != 0)
                {
                    graphicsQueue2()->Flush();
                }
                meshes[key] = std::move(mesh);
                stats.meshCount = static_cast<uint32_t>(meshes.size());
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during registerMesh: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during registerMesh";
            }
            return false;
        }

        bool registerMask(const uint64_t key, const MaskDesc& desc)
        {
            if (!ready || desc.mips == nullptr || desc.mipCount == 0 || maskCount >= MAX_MASKS)
            {
                return false;
            }

            try
            {
                auto device = secondaryDevice->GetDXDevice();
                const auto mipCount = static_cast<UINT16>(desc.mipCount);
                const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(static_cast<DXGI_FORMAT>(desc.dxgiFormat), desc.width,
                                                                                     desc.height, 1, mipCount);
                const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
                Mask mask;
                ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr, IID_PPV_ARGS(&mask.texture)));

                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(desc.mipCount);
                UINT64 uploadSize = 0;
                device->GetCopyableFootprints(&textureDesc, 0, desc.mipCount, 0, footprints.data(), nullptr, nullptr, &uploadSize);

                const auto upload = createUploadBuffer(secondaryDevice, static_cast<size_t>(uploadSize), L"MgpuShadows Mask Upload");
                uint8_t* mapped = nullptr;
                const D3D12_RANGE noRead = {0, 0};
                ThrowIfFailed(upload->GetD3D12Resource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
                for (uint32_t mip = 0; mip < desc.mipCount; ++mip)
                {
                    const MaskMip& source = desc.mips[mip];
                    const auto rowSize = std::min<size_t>(source.rowPitch, footprints[mip].Footprint.RowPitch);
                    for (uint32_t row = 0; row < source.rowCount; ++row)
                    {
                        std::memcpy(mapped + footprints[mip].Offset + static_cast<size_t>(row) * footprints[mip].Footprint.RowPitch,
                                    static_cast<const uint8_t*>(source.data) + static_cast<size_t>(row) * source.rowPitch, rowSize);
                    }
                }
                upload->GetD3D12Resource()->Unmap(0, nullptr);

                auto queue = graphicsQueue2();
                auto cmdList = queue->GetCommandList();
                auto* raw = cmdList->GetGraphicsCommandList().Get();
                for (uint32_t mip = 0; mip < desc.mipCount; ++mip)
                {
                    const CD3DX12_TEXTURE_COPY_LOCATION dst(mask.texture.Get(), mip);
                    const CD3DX12_TEXTURE_COPY_LOCATION src(upload->GetD3D12Resource().Get(), footprints[mip]);
                    raw->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(mask.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                raw->ResourceBarrier(1, &toShaderResource);
                queue->WaitForFenceValue(queue->ExecuteCommandList(cmdList));

                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = static_cast<DXGI_FORMAT>(desc.dxgiFormat);
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = desc.mipCount;
                const CD3DX12_CPU_DESCRIPTOR_HANDLE cpu(maskHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(maskCount), maskDescriptorSize);
                device->CreateShaderResourceView(mask.texture.Get(), &srv, cpu);
                mask.descriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(maskHeap->GetGPUDescriptorHandleForHeapStart(), static_cast<INT>(maskCount), maskDescriptorSize);
                ++maskCount;

                masks[key] = std::move(mask);
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during registerMask: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during registerMask";
            }
            return false;
        }

        void forgetMeshes()
        {
            if (ready)
            {
                flushUploads();
                graphicsQueue2()->Flush();
                uploadsInFlight.clear();
            }
            meshes.clear();
            masks.clear();
            maskCount = 0;
            stats.meshCount = 0;
        }

        bool ensureAtlas(const uint32_t width, const uint32_t height, const AtlasFormat format)
        {
            if (atlas && atlasWidth == width && atlasHeight == height && atlasFormat == format)
            {
                return false;
            }

            if (atlas)
            {
                graphicsQueue2()->Flush();
            }

            const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(typelessFormat(format), width, height, 1, 1, 1, 0,
                                                                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = depthFormat(format);
            clear.DepthStencil.Depth = 1.0f;
            atlas = std::make_shared<gfx::GTexture>(secondaryDevice, desc, L"MgpuShadows Atlas", gfx::TextureUsage::Depth,
                                                    &clear);

            if (atlasDsv.IsNull())
            {
                atlasDsv = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
            }
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = depthFormat(format);
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            atlas->CreateDepthStencilView(&dsvDesc, &atlasDsv);

            atlasWidth = width;
            atlasHeight = height;
            atlasFormat = format;
            return true;
        }

        void ensureSlot(Slot& slot, const FrameDesc& frame)
        {
            if (!slot.shared || slot.width != frame.atlasWidth || slot.height != frame.atlasHeight ||
                slot.format != frame.format)
            {
                D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(typelessFormat(frame.format), frame.atlasWidth,
                                                                        frame.atlasHeight, 1, 1);
                const auto index = static_cast<size_t>(&slot - slots.data());
                slot.shared = std::make_shared<GCrossAdapterResource>(desc, primaryDevice, secondaryDevice,
                                                                      L"MgpuShadows Shared " + std::to_wstring(index));
                const D3D12_RESOURCE_DESC localDesc = CD3DX12_RESOURCE_DESC::Tex2D(typelessFormat(frame.format), frame.atlasWidth,
                                                                                   frame.atlasHeight, 1, 1);
                slot.local = std::make_shared<gfx::GResource>(primaryDevice, localDesc, L"MgpuShadows Local " + std::to_wstring(index));
                slot.width = frame.atlasWidth;
                slot.height = frame.atlasHeight;
                slot.format = frame.format;
            }

            const size_t matricesSize = static_cast<size_t>(frame.matrixCount) * 16 * sizeof(float);
            if (!slot.matrices || slot.matricesCapacity < matricesSize)
            {
                slot.matricesCapacity = std::max<size_t>(matricesSize * 2, 64 * 16 * sizeof(float));
                slot.matrices = createUploadBuffer(secondaryDevice, slot.matricesCapacity, L"MgpuShadows Matrices");
            }
            if (matricesSize != 0)
            {
                writeUploadBuffer(*slot.matrices, frame.matrices, matricesSize);
            }
        }

        Slot* freeSlot()
        {
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Free)
                {
                    return &slot;
                }
            }
            return nullptr;
        }

        bool canSubmit()
        {
            if (!ready)
            {
                return false;
            }
            collectFinished();
            const bool rendering = std::any_of(slots.begin(), slots.end(), [](const Slot& item)
            {
                return item.state == SlotState::Rendering;
            });
            return !rendering && freeSlot() != nullptr;
        }

        bool submit(const FrameDesc& frame)
        {
            if (!ready || frame.atlasWidth == 0 || frame.atlasHeight == 0)
            {
                return false;
            }

            flushUploads();

            collectFinished();
            const bool rendering = std::any_of(slots.begin(), slots.end(), [](const Slot& item)
            {
                return item.state == SlotState::Rendering;
            });
            Slot* slot = rendering ? nullptr : freeSlot();
            if (slot == nullptr)
            {
                ++stats.skippedBusy;
                return false;
            }

            try
            {
                const bool newAtlas = ensureAtlas(frame.atlasWidth, frame.atlasHeight, frame.format);
                ensureSlot(*slot, frame);
                bool keepAny = false;
                for (uint32_t c = 0; c < frame.cascadeCount && !newAtlas; ++c)
                {
                    keepAny = keepAny || frame.cascades[c].keep;
                }

                auto queue = graphicsQueue2();
                auto cmdList = queue->GetCommandList();
                auto* raw = cmdList->GetGraphicsCommandList().Get();
                const UINT firstStamp = slotIndex(*slot) * DRAW_STAMPS;
                if (drawQueries)
                {
                    raw->EndQuery(drawQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp);
                }

                cmdList->TransitionBarrier(*atlas, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                cmdList->FlushResourceBarriers();
                cmdList->SetRenderTargets(0, nullptr, 0, &atlasDsv, 0);
                if (!keepAny)
                {
                    cmdList->ClearDepthStencil(&atlasDsv, 0, D3D12_CLEAR_FLAG_DEPTH, 1.0f);
                }
                else
                {
                    clearRects.clear();
                    for (uint32_t c = 0; c < frame.cascadeCount; ++c)
                    {
                        const CascadeDesc& desc = frame.cascades[c];
                        if (!desc.keep)
                        {
                            clearRects.push_back({static_cast<LONG>(desc.viewport[0]), static_cast<LONG>(desc.viewport[1]),
                                                  static_cast<LONG>(desc.viewport[0] + desc.viewport[2]),
                                                  static_cast<LONG>(desc.viewport[1] + desc.viewport[3])});
                        }
                    }
                    if (!clearRects.empty())
                    {
                        cmdList->ClearDepthStencil(&atlasDsv, 0, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, clearRects.data(),
                                                   clearRects.size());
                    }
                }

                cmdList->SetRootSignature(*signature);
                raw->SetGraphicsRootShaderResourceView(MATRICES_PARAMETER,
                                                       slot->matrices->GetD3D12Resource()->GetGPUVirtualAddress());
                ID3D12DescriptorHeap* const descriptorHeaps[] = {maskHeap.Get()};
                raw->SetDescriptorHeaps(1, descriptorHeaps);
                cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                drawOrder.resize(frame.drawCount);
                for (uint32_t d = 0; d < frame.drawCount; ++d)
                {
                    drawOrder[d] = d;
                }
                std::sort(drawOrder.begin(), drawOrder.end(), [&frame](const uint32_t a, const uint32_t b)
                {
                    const DrawDesc& x = frame.draws[a];
                    const DrawDesc& y = frame.draws[b];
                    return std::make_tuple(x.cascade, x.boneCount != 0, x.mask, x.mesh, a) <
                           std::make_tuple(y.cascade, y.boneCount != 0, y.mask, y.mesh, b);
                });

                uint32_t drawn = 0;
                uint32_t currentCascade = UINT32_MAX;
                const CascadeDesc* cascade = nullptr;
                const gfx::GraphicPSO* currentPso = nullptr;
                const Mask* currentMask = nullptr;
                const Mesh* currentMesh = nullptr;
                int currentStreams = -1;
                for (const uint32_t d : drawOrder)
                {
                    const DrawDesc& draw = frame.draws[d];
                    if (draw.cascade >= frame.cascadeCount || (keepAny && frame.cascades[draw.cascade].keep))
                    {
                        continue;
                    }

                    if (draw.cascade != currentCascade)
                    {
                        currentCascade = draw.cascade;
                        cascade = &frame.cascades[currentCascade];

                        const D3D12_VIEWPORT viewport = {static_cast<float>(cascade->viewport[0]),
                                                         static_cast<float>(cascade->viewport[1]),
                                                         static_cast<float>(cascade->viewport[2]),
                                                         static_cast<float>(cascade->viewport[3]), 0.0f, 1.0f};
                        const D3D12_RECT scissor = {static_cast<LONG>(cascade->viewport[0]), static_cast<LONG>(cascade->viewport[1]),
                                                    static_cast<LONG>(cascade->viewport[0] + cascade->viewport[2]),
                                                    static_cast<LONG>(cascade->viewport[1] + cascade->viewport[3])};
                        cmdList->SetViewports(&viewport, 1);
                        cmdList->SetScissorRects(&scissor, 1);
                        raw->SetGraphicsRoot32BitConstants(CONSTANTS_PARAMETER, VIEW_PROJ_CONSTANTS, cascade->viewProj, 0);
                        currentPso = nullptr;
                    }

                    const auto found = meshes.find(draw.mesh);
                    if (found == meshes.end())
                    {
                        continue;
                    }
                    const Mesh& mesh = found->second;
                    const bool skinned = draw.boneCount != 0;
                    if (skinned && !mesh.skinned)
                    {
                        continue;
                    }

                    const auto foundMask = draw.mask != 0 && !skinned && mesh.texCoords ? masks.find(draw.mask) : masks.end();
                    const bool masked = foundMask != masks.end();

                    const gfx::GraphicPSO& pso = getPso(skinned, masked, frame.format, cascade->depthBias, cascade->slopeScaledDepthBias,
                                                        cascade->depthClip, cascade->flipCull);
                    if (&pso != currentPso)
                    {
                        cmdList->SetPipelineState(pso);
                        currentPso = &pso;
                    }
                    if (masked && &foundMask->second != currentMask)
                    {
                        raw->SetGraphicsRootDescriptorTable(MASK_PARAMETER, foundMask->second.descriptor);
                        currentMask = &foundMask->second;
                    }
                    const uint32_t drawConstants[DRAW_CONSTANTS] = {draw.firstMatrix, draw.boneCount};
                    raw->SetGraphicsRoot32BitConstants(CONSTANTS_PARAMETER, DRAW_CONSTANTS, drawConstants, VIEW_PROJ_CONSTANTS);

                    const int streams = masked ? 2 : skinned ? 1
                                                             : 0;
                    if (&mesh != currentMesh || streams != currentStreams)
                    {
                        if (masked)
                        {
                            cmdList->SetVBuffer(0, 2u, mesh.maskedViews.data());
                        }
                        else
                        {
                            cmdList->SetVBuffer(0, skinned ? 3u : 1u, mesh.vertexViews.data());
                        }
                        cmdList->SetIBuffer(&mesh.indexView);
                        currentMesh = &mesh;
                        currentStreams = streams;
                    }
                    cmdList->DrawIndexed(draw.indexCount, skinned ? 1u : draw.instanceCount, draw.startIndex, 0, 0);
                    ++drawn;
                }

                if (drawQueries)
                {
                    raw->EndQuery(drawQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp + 1);
                }

                cmdList->CopyResource(slot->shared->GetSharedResource(), *atlas);
                cmdList->TransitionBarrier(slot->shared->GetSharedResource(), D3D12_RESOURCE_STATE_COMMON);
                cmdList->TransitionBarrier(*atlas, D3D12_RESOURCE_STATE_COMMON);
                cmdList->FlushResourceBarriers();
                if (drawQueries)
                {
                    raw->EndQuery(drawQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp + 2);
                    raw->ResolveQueryData(drawQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp, DRAW_STAMPS, drawReadback.Get(),
                                          firstStamp * sizeof(UINT64));
                }

                slot->fenceValue = queue->ExecuteCommandList(cmdList);
                slot->state = SlotState::Rendering;
                slot->sequence = ++nextSequence;
                slot->userToken = frame.userToken;
                slot->submitted = Clock::now();

                ++stats.submitted;
                stats.lastDrawCount = drawn;
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during submit: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during submit (see the D3D12 debug output)";
            }

            ++stats.failed;
            return false;
        }

        void startTransfer(Slot& slot)
        {
            try
            {
                auto queue = copyQueue1();
                auto cmdList = queue->GetCommandList();
                auto* raw = cmdList->GetGraphicsCommandList().Get();
                const UINT firstStamp = slotIndex(slot) * TRANSFER_STAMPS;
                if (transferQueries)
                {
                    raw->EndQuery(transferQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp);
                }
                const gfx::GResource& prime = slot.shared->GetPrimeResource();
                cmdList->CopyResource(*slot.local, prime);
                cmdList->TransitionBarrier(*slot.local, D3D12_RESOURCE_STATE_COMMON);
                cmdList->TransitionBarrier(prime, D3D12_RESOURCE_STATE_COMMON);
                cmdList->FlushResourceBarriers();
                if (transferQueries)
                {
                    raw->EndQuery(transferQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp + 1);
                    raw->ResolveQueryData(transferQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstStamp, TRANSFER_STAMPS,
                                          transferReadback.Get(), firstStamp * sizeof(UINT64));
                }
                slot.transferFenceValue = queue->ExecuteCommandList(cmdList);
                slot.state = SlotState::Transferring;
                slot.drawn = Clock::now();
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during the transfer to the first GPU: ") + e.what();
                slot.state = SlotState::Free;
                ++stats.failed;
            }
        }

        void collectFinished()
        {
            releaseFinishedUploads();
            auto graphics2 = graphicsQueue2();
            auto copy1 = copyQueue1();
            bool transferring = false;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Rendering && graphics2->IsFinish(slot.fenceValue))
                {
                    slot.state = SlotState::Drawn;
                    readDrawTimings(slot);
                }
                else if (slot.state == SlotState::Transferring)
                {
                    if (copy1->IsFinish(slot.transferFenceValue))
                    {
                        const Clock::time_point now = Clock::now();
                        slot.state = SlotState::Ready;
                        slot.latencyMs = std::chrono::duration<float, std::milli>(now - slot.submitted).count();
                        stats.lastLatencyMs = slot.latencyMs;
                        stats.lastTransferMs = std::chrono::duration<float, std::milli>(now - slot.drawn).count();
                        ++stats.completed;
                        readTransferTimings(slot);
                    }
                    else
                    {
                        transferring = true;
                    }
                }
            }

            if (transferring)
            {
                return;
            }

            Slot* newest = nullptr;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Drawn && (newest == nullptr || slot.sequence > newest->sequence))
                {
                    newest = &slot;
                }
            }
            if (newest == nullptr)
            {
                return;
            }
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Drawn && &slot != newest)
                {
                    slot.state = SlotState::Free;
                }
            }
            startTransfer(*newest);
        }

        bool takeReady(ReadyAtlas& out)
        {
            if (!ready)
            {
                return false;
            }

            collectFinished();

            Slot* newest = nullptr;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Ready && (newest == nullptr || slot.sequence > newest->sequence))
                {
                    newest = &slot;
                }
            }
            if (newest == nullptr)
            {
                return false;
            }

            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Ready && &slot != newest)
                {
                    slot.state = SlotState::Free;
                }
            }

            newest->state = SlotState::Handed;
            out.slot = static_cast<uint32_t>(newest - slots.data());
            out.resource = newest->local->GetD3D12Resource().Get();
            out.width = newest->width;
            out.height = newest->height;
            out.format = newest->format;
            out.userToken = newest->userToken;
            out.latencyMs = newest->latencyMs;
            return true;
        }

        void releaseSlot(const uint32_t index)
        {
            if (index < SLOT_COUNT && slots[index].state == SlotState::Handed)
            {
                slots[index].state = SlotState::Free;
            }
        }

        bool waitAndReadBack(std::vector<float>& depths, uint32_t& width, uint32_t& height)
        {
            if (!ready)
            {
                return false;
            }

            try
            {
                for (;;)
                {
                    collectFinished();
                    if (std::any_of(slots.begin(), slots.end(), [](const Slot& item)
                    {
                        return item.state == SlotState::Ready;
                    }))
                    {
                        break;
                    }

                    const auto busy = std::find_if(slots.begin(), slots.end(), [](const Slot& item)
                    {
                        return item.state == SlotState::Rendering || item.state == SlotState::Transferring;
                    });
                    if (busy == slots.end())
                    {
                        break;
                    }
                    if (busy->state == SlotState::Rendering)
                    {
                        graphicsQueue2()->WaitForFenceValue(busy->fenceValue);
                    }
                    else
                    {
                        copyQueue1()->WaitForFenceValue(busy->transferFenceValue);
                    }
                }

                ReadyAtlas readyAtlas;
                if (!takeReady(readyAtlas))
                {
                    error = "no atlas was submitted";
                    return false;
                }
                const Slot& slot = slots[readyAtlas.slot];

                const gfx::GResource& local = *slot.local;
                const D3D12_RESOURCE_DESC localDesc = local.GetD3D12Resource()->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
                UINT64 size = 0;
                primaryDevice->GetDXDevice()->GetCopyableFootprints(&localDesc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
                gfx::GResource readback(primaryDevice, CD3DX12_RESOURCE_DESC::Buffer(size), L"MgpuShadows Readback", nullptr,
                                        D3D12_RESOURCE_STATE_COPY_DEST, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK));

                auto copyQueue = primaryDevice->GetCommandQueue(gfx::GQueueType::Copy);
                auto cmdList = copyQueue->GetCommandList();
                cmdList->TransitionBarrier(local, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmdList->FlushResourceBarriers();
                const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.GetD3D12Resource().Get(), footprint);
                const CD3DX12_TEXTURE_COPY_LOCATION src(local.GetD3D12Resource().Get(), 0);
                cmdList->GetGraphicsCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                cmdList->TransitionBarrier(local, D3D12_RESOURCE_STATE_COMMON);
                cmdList->FlushResourceBarriers();
                copyQueue->WaitForFenceValue(copyQueue->ExecuteCommandList(cmdList));

                width = slot.width;
                height = slot.height;
                depths.assign(static_cast<size_t>(width) * height, 0.0f);

                const uint8_t* mapped = nullptr;
                const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(size)};
                ThrowIfFailed(readback.GetD3D12Resource()->Map(0, &readRange,
                                                               reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
                const uint32_t texel = bytesPerTexel(slot.format);
                for (uint32_t y = 0; y < height; ++y)
                {
                    const uint8_t* row = mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        float value = 0.0f;
                        if (texel == 2)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, row + x * texel, sizeof(raw));
                            value = static_cast<float>(raw) / 65535.0f;
                        }
                        else
                        {
                            std::memcpy(&value, row + x * texel, sizeof(value));
                        }
                        depths[static_cast<size_t>(y) * width + x] = value;
                    }
                }
                const D3D12_RANGE noWrite = {0, 0};
                readback.GetD3D12Resource()->Unmap(0, &noWrite);

                releaseSlot(readyAtlas.slot);
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during read-back: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during read-back (see the D3D12 debug output)";
            }
            return false;
        }

        void release()
        {
            ready = false;

            if (primaryDevice)
            {
                primaryDevice->Flush();
            }
            if (secondaryDevice)
            {
                secondaryDevice->Flush();
            }

            for (Slot& slot : slots)
            {
                slot = Slot();
            }
            releaseTimings();
            uploadList.reset();
            uploadBuffers.clear();
            uploadsInFlight.clear();
            meshes.clear();
            psos.clear();
            staticVS.reset();
            skinnedVS.reset();
            maskedVS.reset();
            maskedPS.reset();
            masks.clear();
            maskCount = 0;
            maskHeap.Reset();
            signature.reset();
            atlasDsv = gfx::GDescriptor();
            atlas.reset();
            atlasWidth = 0;
            atlasHeight = 0;
            primaryDevice.reset();
            secondaryDevice.reset();
            stats.meshCount = 0;
        }
    };

    CascadeRenderer::CascadeRenderer() :
        m_impl(std::make_unique<Impl>())
    {
    }

    CascadeRenderer::~CascadeRenderer()
    {
        shutdown();
    }

    bool CascadeRenderer::initialize(const char* nauAdapterName)
    {
        if (m_impl->ready)
        {
            return true;
        }
        return m_impl->initialize(nauAdapterName);
    }

    bool CascadeRenderer::isReady() const
    {
        return m_impl->ready;
    }

    void CascadeRenderer::shutdown()
    {
        if (m_impl)
        {
            m_impl->release();
        }
    }

    bool CascadeRenderer::hasMesh(const uint64_t key) const
    {
        return m_impl->meshes.count(key) != 0;
    }

    bool CascadeRenderer::registerMesh(const uint64_t key, const MeshDesc& mesh)
    {
        return m_impl->registerMesh(key, mesh);
    }

    bool CascadeRenderer::registerMask(const uint64_t key, const MaskDesc& mask)
    {
        return m_impl->registerMask(key, mask);
    }

    void CascadeRenderer::forgetMeshes()
    {
        m_impl->forgetMeshes();
    }

    bool CascadeRenderer::submit(const FrameDesc& frame)
    {
        return m_impl->submit(frame);
    }

    bool CascadeRenderer::canSubmit()
    {
        return m_impl->canSubmit();
    }

    bool CascadeRenderer::takeReady(ReadyAtlas& out)
    {
        return m_impl->takeReady(out);
    }

    void CascadeRenderer::release(const uint32_t slot)
    {
        m_impl->releaseSlot(slot);
    }

    bool CascadeRenderer::waitAndReadBack(std::vector<float>& depths, uint32_t& width, uint32_t& height)
    {
        return m_impl->waitAndReadBack(depths, width, height);
    }

    const char* CascadeRenderer::lastError() const
    {
        return m_impl->error.c_str();
    }

    const char* CascadeRenderer::primaryAdapterName() const
    {
        return m_impl->primaryName.c_str();
    }

    const char* CascadeRenderer::secondaryAdapterName() const
    {
        return m_impl->secondaryName.c_str();
    }

    const RendererStats& CascadeRenderer::stats() const
    {
        return m_impl->stats;
    }
}  // namespace nau::mgpu::shadows
