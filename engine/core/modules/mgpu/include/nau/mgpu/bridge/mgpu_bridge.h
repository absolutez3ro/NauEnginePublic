#pragma once

// Facade between Nau and the Mgpu module.
//
// Only plain C++ types are exposed here on purpose: Mgpu headers must not be included into Nau code
// (global `using namespace DirectX::SimpleMath`, the `PI` macro, /permissive, static CRT).
//
// Transfer path (see mgpu_bridge.cpp):
//   GPU2 (compute queue): compute shader draws an animated sine plasma -> texture -> ::GCrossAdapterResource
//   (shared heap); the CPU does not produce the image
//   GPU1 (Mgpu device on Nau's adapter, copy queue): shared heap -> read-back buffer -> CPU pixels
// The CPU pixels are then written by the caller into a Nau texture (BaseTexture::lockimg).
// pump() keeps several images in flight and never waits, so the second GPU works in parallel with Nau's frame.
//
// Without NAU_ENABLE_MGPU (the Mgpu target defines it publicly) this header provides inline stubs, so Nau code
// can use the same calls whether or not Mgpu is linked.

#include <cstdint>
#include <memory>

namespace nau::mgpu::bridge
{
    struct TransferInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0; // bytes per row of pixels(); pixels are tightly packed RGBA8
        uint64_t checksum = 0; // FNV-1a of the pixels GPU1 received through the shared heap
        uint64_t expectedChecksum = 0; // FNV-1a of the pixels GPU2 produced (read back on GPU2)
        // true when the received pixels equal the produced ones bit-exactly and the image is not empty/flat
        bool checksumMatches = false;
    };

    struct TransferStats
    {
        uint64_t imagesOk = 0;
        uint64_t imagesFailed = 0;
        float lastLatencyMs = 0; // start of the image on GPU2 -> its pixels on the CPU
    };

    /// Snapshot for UI: what the Mgpu path is doing right now. The render side publishes it (publishStatus()),
    /// any thread may read it (getStatus()).
    struct Status
    {
        bool linked = false; // built with NAU_ENABLE_MGPU
        bool enabled = false; // the setEnabled() switch
        bool ready = false; // two hardware adapters found, the second GPU is initialized
        char primaryAdapter[128] = {};
        char secondaryAdapter[128] = {};
        char lastError[256] = {};
        float sceneFramesPerSecond = 0; // frames Nau renders per second (ImGui's frame rate is the update loop's)

        // Shadow cascades drawn by the second GPU (mgpu_shadows.h).
        bool shadowsFromSecondGpu = false; // the last frame used an atlas of the second GPU, not Nau's own cascades
        uint32_t atlasAgeFrames = 0; // Nau frames between drawing that atlas and using it
        float atlasesPerSecond = 0; // atlases of the second GPU copied into Nau's atlas per second
        float latencyMs = 0; // submit -> atlas finished on the second GPU
        uint32_t drawCount = 0; // draws in the last atlas
        uint64_t atlasesCompleted = 0;
        uint64_t skippedBusy = 0; // frames that found the second GPU still busy with the previous atlases
        uint64_t failed = 0;

        // Where the time of a frame goes (stage 0 of the MGPU optimisation): averages over the last report period. CPU:
        // render thread, milliseconds per frame. GPU: milliseconds per measured pass, -1 when nothing was measured.
        struct Timings
        {
            float cpuFrameMs = 0; // the whole frame (1000 / sceneFramesPerSecond)
            float cpuRenderMs = 0; // the render thread's own work in a frame: the views and the frame graph, without present
            float cpuListsMainMs = 0; // render lists of the camera view
            float cpuListsCascadesMs = 0; // of the cascade views
            float cpuListsOmniMs = 0; // of the point light views
            uint32_t instancesMain = 0; // instances in the lists of a frame
            uint32_t instancesCascades = 0;
            uint32_t instancesOmni = 0;
            uint32_t instancesPerCascade[6] = {}; // by cascade view (csm::CascadeShadows::MAX_CASCADES)
            float cpuCollectMs = 0; // the casters for the second GPU, per frame
            float cpuSubmitMs = 0; // recording and submitting its atlas, per frame
            float gpuFrameMs = -1; // Nau's frame graph on the scene GPU
            float gpuNauCascadesMs = -1; // Nau's own cascade pass
            float gpuAtlasCopyMs = -1; // an atlas of the second GPU into Nau's atlas
            float gpu2DrawMs = -1; // the cascades on the second GPU, per atlas
            float gpu2SharedCopyMs = -1; // the atlas into the shared heap on the second GPU
            float gpuTransferMs = -1; // shared heap -> the scene GPU on its copy queue, per atlas
        };
        Timings timings;
    };

#if defined(NAU_ENABLE_MGPU)

    /// Manual switch of the Mgpu path. Default: false.
    void setEnabled(bool enabled);
    bool isEnabled();

    /// Vsync of Nau's swap chain (SceneBaseSample: key V). With it on the frame rate is the one of the display.
    void setVsyncEnabled(bool enabled);
    bool isVsyncEnabled();

    /// Nau has replaced its meshes (another scene): the second GPU forgets its copies. Every call changes the generation.
    void notifyMeshesChanged();
    uint64_t getMeshGeneration();

    /// linked and enabled are filled in by getStatus() itself.
    void publishStatus(const Status& status);
    Status getStatus();

    class TransferBridge
    {
    public:
        TransferBridge();
        ~TransferBridge();

        TransferBridge(const TransferBridge&) = delete;
        TransferBridge& operator=(const TransferBridge&) = delete;

        /// nauAdapterName: name of the adapter Nau renders on (d3d::get_device_name()); it selects the primary
        /// device. Needs two distinct hardware adapters; returns false (see lastError()) otherwise.
        bool initialize(const char* nauAdapterName);

        /// Non-blocking. Collects the images that have arrived on GPU1 (each one is checked) and, if a slot is free,
        /// starts the next image on GPU2. frameIndex is the animation phase of the image. Returns true when a new
        /// verified image is available in pixels().
        bool pump(uint32_t frameIndex);

        /// Blocking: runs one full transfer and waits for it (used by the standalone test).
        /// Returns true when the transfer completed and the received pixels match what GPU2 produced.
        bool update(uint32_t frameIndex);

        bool isReady() const;
        void shutdown();

        const char* lastError() const;
        const char* primaryAdapterName() const;
        const char* secondaryAdapterName() const;

        /// Tightly packed RGBA8 pixels of the last verified image, info().width * info().height * 4 bytes.
        const void* pixels() const;
        const TransferInfo& info() const;
        const TransferStats& stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

#else

    inline void setEnabled(bool) {}
    inline bool isEnabled() { return false; }
    inline void setVsyncEnabled(bool) {}
    inline bool isVsyncEnabled() { return true; }
    inline void notifyMeshesChanged() {}
    inline uint64_t getMeshGeneration() { return 0; }
    inline void publishStatus(const Status&) {}
    inline Status getStatus() { return {}; }

    class TransferBridge
    {
    public:
        bool initialize(const char*) { return false; }
        bool pump(uint32_t) { return false; }
        bool update(uint32_t) { return false; }
        bool isReady() const { return false; }
        void shutdown() {}
        const char* lastError() const { return "Mgpu is not linked (NAU_ENABLE_MGPU is off)"; }
        const char* primaryAdapterName() const { return ""; }
        const char* secondaryAdapterName() const { return ""; }
        const void* pixels() const { return nullptr; }
        const TransferInfo& info() const { return m_info; }
        const TransferStats& stats() const { return m_stats; }

    private:
        TransferInfo m_info;
        TransferStats m_stats;
    };

#endif
}
