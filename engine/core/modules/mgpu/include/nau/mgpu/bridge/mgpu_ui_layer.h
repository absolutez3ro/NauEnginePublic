#pragma once

// Second-GPU renderer of the UI layer (ui_layer in graphics/src/render_window/render_graph.cpp).
//
// GPU1 draws the UI into a premultiplied RGBA8 texture of the window size and copies it into a cross-adapter shared
// heap; the copy queue of GPU0 then moves it into local memory, and takeReady() returns that texture, which Nau copies
// into its own texture and composites over the frame (ui_composite). The transfer is the one of the shadow atlas
// (mgpu_shadows.h).
//
// Stage A (now): GPU1 clears the layer to transparent and fills one rectangle with a test colour, to check the path
// GPU1 -> GPU0 -> screen. ImGui and cocos2d-x are drawn into the same layer in the next stages.
//
// Asynchronous by default, as the shadows: submit() and takeReady() never wait; the caller takes the newest layer
// that has arrived (a few frames old) before handing the next frame to GPU1.
// FrameDesc::waitForResult = true makes submit() synchronous: it returns after the layer of this frame is in local
// memory of GPU0, so takeReady() gives the layer of the same frame (age 0). Slower; for debugging what GPU1 draws.
//
// Only plain C++ types here, see mgpu_bridge.h. Without NAU_ENABLE_MGPU this header provides inline stubs.

#include <cstdint>
#include <memory>

namespace nau::mgpu::ui_layer
{
    /// Layers in flight: drawn on GPU1, moved to GPU0, ready, or handed to Nau (ReadyLayer::slot < SLOT_COUNT).
    /// Four, as the shadow atlases: a handed slot waits until Nau's frame with the copy has finished on the GPU, so in
    /// the asynchronous mode up to two slots may be handed while one is drawn and one is ready.
    constexpr uint32_t SLOT_COUNT = 4;

    /// Which queues the layer uses (chosen once, in initialize()).
    enum class QueueMode : uint32_t
    {
        // The queues of the Mgpu devices, shared with the shadow and DOF bridges: graphics queue of GPU1, copy queue
        // of GPU0. A queue runs its work in order, so the layer waits behind the shadow atlas and DOF.
        Shared,
        // Own graphics queue on GPU1 and own copy queue on GPU0, normal priority: the layer does not wait in line
        // behind the other bridges (the GPU still divides its time between the queues).
        Own,
        // Own queues with D3D12_COMMAND_QUEUE_PRIORITY_HIGH: the scheduler may prefer the layer over the other
        // bridges (the driver decides; on some GPUs it changes nothing).
        OwnHigh
    };

    inline const char* toString(const QueueMode mode)
    {
        switch (mode)
        {
            case QueueMode::Own:
                return "own";
            case QueueMode::OwnHigh:
                return "own, high priority";
            default:
                return "shared";
        }
    }

    struct FrameDesc
    {
        uint32_t width = 0; // layer size in pixels, the size of Nau's window
        uint32_t height = 0;

        // Stage A test content: the whole layer is cleared to transparent, then this rectangle is filled with this
        // colour. Premultiplied alpha (rgb already multiplied by a), as ui_composite expects.
        float fillColor[4] = {};
        uint32_t fillRect[4] = {}; // x, y, width, height in layer pixels (y from the top); width 0 = no rectangle

        uint64_t userToken = 0; // returned with the layer, e.g. Nau's frame number
        bool waitForResult = false; // synchronous submit(), see above
    };

    struct ReadyLayer
    {
        uint32_t slot = 0;
        // ID3D12Resource* in local memory of Nau's adapter (on its device), D3D12_RESOURCE_STATE_COMMON,
        // DXGI_FORMAT_R8G8B8A8_UNORM (the format drv3d_DX12 creates TEXFMT_R8G8B8A8 | TEXCF_RTARGET textures with).
        // Stays valid until release(slot).
        void* resource = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t userToken = 0;
        float latencyMs = 0; // submit() -> in local memory of GPU0, observed on the CPU
    };

    struct RendererStats
    {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t skippedBusy = 0; // submit() calls while GPU1 was still drawing, or every slot was in use
        uint64_t failed = 0;
        float lastLatencyMs = 0;
        float lastTransferMs = 0; // shared heap -> local memory of GPU0 (its copy queue)
    };

#if defined(NAU_ENABLE_MGPU)

    class LayerRenderer
    {
    public:
        LayerRenderer();
        ~LayerRenderer();

        LayerRenderer(const LayerRenderer&) = delete;
        LayerRenderer& operator=(const LayerRenderer&) = delete;

        /// nauAdapterName: d3d::get_device_name(). Needs two distinct hardware adapters.
        bool initialize(const char* nauAdapterName, QueueMode queueMode = QueueMode::Shared);
        bool isReady() const;
        /// Waits for the work in flight on both GPUs and releases everything.
        void shutdown();

        /// Starts drawing the layer on GPU1 if it has finished the previous one and a slot is free; returns false
        /// (nothing submitted) otherwise. With FrameDesc::waitForResult it also waits until the layer is in local
        /// memory of GPU0.
        bool submit(const FrameDesc& frame);

        /// Non-blocking. The newest layer that has arrived in local memory of GPU0 and was not handed out yet; older
        /// ones are dropped. The slot stays reserved until release(): the caller's copy must have executed by then.
        bool takeReady(ReadyLayer& out);
        void release(uint32_t slot);

        const char* lastError() const;
        const char* primaryAdapterName() const;
        const char* secondaryAdapterName() const;
        const RendererStats& stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

#else

    class LayerRenderer
    {
    public:
        bool initialize(const char*, QueueMode = QueueMode::Shared) { return false; }
        bool isReady() const { return false; }
        void shutdown() {}
        bool submit(const FrameDesc&) { return false; }
        bool takeReady(ReadyLayer&) { return false; }
        void release(uint32_t) {}
        const char* lastError() const { return "Mgpu is not linked (NAU_ENABLE_MGPU is off)"; }
        const char* primaryAdapterName() const { return ""; }
        const char* secondaryAdapterName() const { return ""; }
        const RendererStats& stats() const { return m_stats; }

    private:
        RendererStats m_stats;
    };

#endif
}
