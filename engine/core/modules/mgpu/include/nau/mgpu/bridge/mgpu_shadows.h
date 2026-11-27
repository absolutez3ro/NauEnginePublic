#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace nau::mgpu::shadows
{
    constexpr uint32_t SLOT_COUNT = 4;

    enum class AtlasFormat : uint32_t
    {
        Depth16,
        Depth32
    };

    struct MeshDesc
    {
        const float* positions = nullptr;
        uint32_t vertexCount = 0;
        const void* indices = nullptr;
        uint32_t indexCount = 0;
        bool indices32 = false;
        const float* boneWeights = nullptr;
        const uint32_t* boneIndices = nullptr;
        const float* texCoords = nullptr;
    };

    struct MaskMip
    {
        const void* data = nullptr;
        uint32_t rowPitch = 0;
        uint32_t rowCount = 0;
    };

    struct MaskDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t dxgiFormat = 0;
        const MaskMip* mips = nullptr;
        uint32_t mipCount = 0;
    };

    struct CascadeDesc
    {
        float viewProj[16] = {};
        uint32_t viewport[4] = {};
        int32_t depthBias = 0;
        float slopeScaledDepthBias = 0;
        bool depthClip = false;
        bool flipCull = false;
        bool keep = false;
    };

    struct DrawDesc
    {
        uint64_t mesh = 0;
        uint32_t cascade = 0;
        uint32_t startIndex = 0;
        uint32_t indexCount = 0;
        uint32_t firstMatrix = 0;
        uint32_t instanceCount = 1;
        uint32_t boneCount = 0;
        uint64_t mask = 0;
    };

    struct FrameDesc
    {
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        AtlasFormat format = AtlasFormat::Depth16;
        const CascadeDesc* cascades = nullptr;
        uint32_t cascadeCount = 0;
        const DrawDesc* draws = nullptr;
        uint32_t drawCount = 0;
        const float* matrices = nullptr;
        uint32_t matrixCount = 0;
        uint64_t userToken = 0;
    };

    struct ReadyAtlas
    {
        uint32_t slot = 0;
        void* resource = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        AtlasFormat format = AtlasFormat::Depth16;
        uint64_t userToken = 0;
        float latencyMs = 0;
    };

    struct RendererStats
    {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t skippedBusy = 0;
        uint64_t failed = 0;
        uint32_t lastDrawCount = 0;
        uint32_t meshCount = 0;
        float lastLatencyMs = 0;
        float lastTransferMs = 0;

        double drawGpuMsTotal = 0;
        double sharedCopyGpuMsTotal = 0;
        uint64_t drawGpuSamples = 0;
        double transferGpuMsTotal = 0;
        uint64_t transferGpuSamples = 0;
    };

#if defined(NAU_ENABLE_MGPU)

    class CascadeRenderer
    {
    public:
        CascadeRenderer();
        ~CascadeRenderer();

        CascadeRenderer(const CascadeRenderer&) = delete;
        CascadeRenderer& operator=(const CascadeRenderer&) = delete;

        bool initialize(const char* nauAdapterName);
        bool isReady() const;
        void shutdown();

        bool hasMesh(uint64_t key) const;
        bool registerMesh(uint64_t key, const MeshDesc& mesh);
        bool registerMask(uint64_t key, const MaskDesc& mask);
        void forgetMeshes();

        bool submit(const FrameDesc& frame);
        bool canSubmit();

        bool takeReady(ReadyAtlas& out);
        void release(uint32_t slot);

        bool waitAndReadBack(std::vector<float>& depths, uint32_t& width, uint32_t& height);

        const char* lastError() const;
        const char* primaryAdapterName() const;
        const char* secondaryAdapterName() const;
        const RendererStats& stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

#else

    class CascadeRenderer
    {
    public:
        bool initialize(const char*)
        {
            return false;
        }
        bool isReady() const
        {
            return false;
        }
        void shutdown()
        {
        }
        bool hasMesh(uint64_t) const
        {
            return false;
        }
        bool registerMesh(uint64_t, const MeshDesc&)
        {
            return false;
        }
        bool registerMask(uint64_t, const MaskDesc&)
        {
            return false;
        }
        void forgetMeshes()
        {
        }
        bool submit(const FrameDesc&)
        {
            return false;
        }
        bool canSubmit()
        {
            return false;
        }
        bool takeReady(ReadyAtlas&)
        {
            return false;
        }
        void release(uint32_t)
        {
        }
        bool waitAndReadBack(std::vector<float>&, uint32_t&, uint32_t&)
        {
            return false;
        }
        const char* lastError() const
        {
            return "Mgpu is not linked (NAU_ENABLE_MGPU is off)";
        }
        const char* primaryAdapterName() const
        {
            return "";
        }
        const char* secondaryAdapterName() const
        {
            return "";
        }
        const RendererStats& stats() const
        {
            return m_stats;
        }

    private:
        RendererStats m_stats;
    };

#endif
}  // namespace nau::mgpu::shadows
