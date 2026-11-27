#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace nau::mgpu::bridge
{
    struct FrameTiming
    {
        double frameTimeMs = 0.0;
        bool cascadesFromSecondGpu = false;
        bool omniShadowsFromSecondGpu = false;
        uint32_t cascadeAtlasAgeFrames = 0;
    };

    struct FrameTimingCapture
    {
        std::vector<FrameTiming> frames;
        bool overflowed = false;
    };

    struct TransferInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;  // bytes per row of pixels(); pixels are tightly packed RGBA8
        uint64_t checksum = 0;
        uint64_t expectedChecksum = 0;
        bool checksumMatches = false;
    };

    struct TransferStats
    {
        uint64_t imagesOk = 0;
        uint64_t imagesFailed = 0;
        float lastLatencyMs = 0;
    };

    struct Status
    {
        bool linked = false;
        bool enabled = false;
        bool autoMode = false;
        bool autoSuspended = false;
        float autoOtherFramesPerSecond = 0;
        bool ready = false;
        char primaryAdapter[128] = {};
        char secondaryAdapter[128] = {};
        char lastError[256] = {};
        float sceneFramesPerSecond = 0;

        bool shadowsFromSecondGpu = false;
        uint32_t atlasAgeFrames = 0;
        float atlasesPerSecond = 0;
        float latencyMs = 0;
        uint32_t drawCount = 0;
        uint64_t atlasesCompleted = 0;
        uint64_t skippedBusy = 0;
        uint64_t failed = 0;

        struct Timings
        {
            float cpuFrameMs = 0;
            float cpuRenderMs = 0;
            float cpuListsMainMs = 0;
            float cpuListsCascadesMs = 0;
            float cpuListsOmniMs = 0;
            uint32_t instancesMain = 0;
            uint32_t instancesCascades = 0;
            uint32_t instancesOmni = 0;
            uint32_t instancesPerCascade[6] = {};
            float cpuCollectMs = 0;
            float cpuSubmitMs = 0;
            float gpuFrameMs = -1;
            float gpuNauCascadesMs = -1;
            float gpuAtlasCopyMs = -1;
            float gpu2DrawMs = -1;
            float gpu2SharedCopyMs = -1;
            float gpuTransferMs = -1;
        };
        Timings timings;
    };

#if defined(NAU_ENABLE_MGPU)

    void setEnabled(bool enabled);
    bool isEnabled();

    struct Settings
    {
        bool autoMode = false;
        uint32_t nearCascades = 2;
        uint32_t maxAtlasesPerSecond = 30;
    };
    void setSettings(const Settings& settings);
    Settings getSettings();

    void setAutoEnabled(bool enabled);
    bool isAutoEnabled();

    void setVsyncEnabled(bool enabled);
    bool isVsyncEnabled();

    void notifyMeshesChanged();
    uint64_t getMeshGeneration();

    void publishStatus(const Status& status);
    Status getStatus();

    void startFrameTimingCapture();
    FrameTimingCapture stopFrameTimingCapture();
    void recordFrameTiming(bool cascadesFromSecondGpu, bool omniShadowsFromSecondGpu, uint32_t cascadeAtlasAgeFrames);

    class TransferBridge
    {
    public:
        TransferBridge();
        ~TransferBridge();

        TransferBridge(const TransferBridge&) = delete;
        TransferBridge& operator=(const TransferBridge&) = delete;

        bool initialize(const char* nauAdapterName);

        bool pump(uint32_t frameIndex);

        bool update(uint32_t frameIndex);

        bool isReady() const;
        void shutdown();

        const char* lastError() const;
        const char* primaryAdapterName() const;
        const char* secondaryAdapterName() const;

        const void* pixels() const;
        const TransferInfo& info() const;
        const TransferStats& stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

#else

    inline void setEnabled(bool)
    {
    }
    inline bool isEnabled()
    {
        return false;
    }
    struct Settings
    {
        bool autoMode = false;
        uint32_t nearCascades = 2;
        uint32_t maxAtlasesPerSecond = 30;
    };
    inline void setSettings(const Settings&)
    {
    }
    inline Settings getSettings()
    {
        return {};
    }
    inline void setAutoEnabled(bool)
    {
    }
    inline bool isAutoEnabled()
    {
        return false;
    }
    inline void setVsyncEnabled(bool)
    {
    }
    inline bool isVsyncEnabled()
    {
        return true;
    }
    inline void notifyMeshesChanged()
    {
    }
    inline uint64_t getMeshGeneration()
    {
        return 0;
    }
    inline void publishStatus(const Status&)
    {
    }
    inline Status getStatus()
    {
        return {};
    }
    inline void startFrameTimingCapture()
    {
    }
    inline FrameTimingCapture stopFrameTimingCapture()
    {
        return {};
    }

    class TransferBridge
    {
    public:
        bool initialize(const char*)
        {
            return false;
        }
        bool pump(uint32_t)
        {
            return false;
        }
        bool update(uint32_t)
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
        const void* pixels() const
        {
            return nullptr;
        }
        const TransferInfo& info() const
        {
            return m_info;
        }
        const TransferStats& stats() const
        {
            return m_stats;
        }

    private:
        TransferInfo m_info;
        TransferStats m_stats;
    };

#endif
}  // namespace nau::mgpu::bridge
