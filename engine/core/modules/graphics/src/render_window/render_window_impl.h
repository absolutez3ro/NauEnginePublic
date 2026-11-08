// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "graphics_nodes.h"
#include "graphics_scene.h"
#include "nau/async/work_queue.h"
#include "nau/math/dag_color.h"
#include "nau/render/deferredRenderer.h"
#include "nau/render/environmentRenderer.h"
#include "nau/render/render_window.h"
#include "render/daBfg/bfg.h"

#if defined(NAU_ENABLE_MGPU)
#include <chrono>

#include <EASTL/array.h>
#include <EASTL/map.h>
#include <EASTL/vector.h>

#include "nau/3d/dag_resPtr.h"
#include "nau/mgpu/bridge/mgpu_bridge.h"
#include "nau/mgpu/bridge/mgpu_shadows.h"
#include "nau/mgpu/bridge/dof_bridge.h"
#include "gpu_timer.h"
#include "nau/osApiWrappers/dag_events.h"
#endif

namespace nau
{
    class GraphicsImpl;
}

namespace nau::render
{
    class RenderWindowImpl : public IRenderWindow, public nau::csm::ICascadeShadowsClient
    {
        NAU_CLASS(nau::render::RenderWindowImpl, rtti::RCPolicy::Concurrent, IRenderWindow);

    public:
        void setWorld(nau::Uid world) override;
        nau::Uid getWorld() override;

        async::Task<> requestViewportResize(int32_t width, int32_t height) override;
        async::Task<> requestViewportResizeImmediate(int32_t width, int32_t height) override;
        void getViewportSize(int32_t& width, int32_t& height) override;

        virtual void * getHwnd() const override;
        eastl::string_view getName() const override;

        virtual void initialize(eastl::string_view name, SWAPID swapchain, void* hwnd) override;
        
        virtual SWAPID getSwapchain() const override;
        eastl::shared_ptr<nau::csm::CascadeShadows> getCSM();

        void setDrawViewportGrid(bool isDrawGrid) override;
        bool getDrawViewportGrid() override;


        // Inherited via ICascadeShadowsClient
        void getCascadeShadowAnchorPoint(float cascade_from, nau::math::Vector3& out_anchor) override;
        void getCascadeShadowSparseUpdateParams(int cascade_no, const nau::math::NauFrustum& cascade_frustum, float& out_min_sparse_dist, int& out_min_sparse_frame) override;
        void renderCascadeShadowDepth(int cascade, const nau::math::Vector2 &znzf) override;

    public:
        void setWorkQueue(WorkQueue::Ptr workQueue);

        int32_t getMainCameraIndex() override;
        void setMainCameraIndex(int32_t ind) override;

        async::Task<> createRenderGraph();

        void render();

#if defined(NAU_ENABLE_MGPU)
        // Around Nau's frame graph (GraphicsImpl::renderMainScene): the GPU time of the frame, the render thread's work.
        void beginFrameTiming();
        void endFrameTiming();
#endif

        virtual async::Task<nau::Uid> requestUidByCoords(int32_t viewportX, int32_t viewportY) override;

        void setOutlineWidth(float newWidth) override;
        void setOutlineColor(const math::Color4&) override;

        virtual async::Task<> enableRenderStages(::nau::TypedFlag<NauRenderStage> stages) override;
        virtual async::Task<> disableRenderStages(::nau::TypedFlag<NauRenderStage> stages) override;

        void setRenderScene(eastl::shared_ptr<GraphicsScene> gScene);

    private:
        void createGBufferNodes();
        void createOutlineNodes();

        // Applies the initial Mgpu switch (MGPU_SHADOWS_ENABLED). The Mgpu path has no nodes of its own: it works inside
        // the CSM and gbuffer_resolve nodes. Without NAU_ENABLE_MGPU it does nothing.
        void initializeMgpu();

        void resizeResolutions();

    private:
        void setName(eastl::string_view name);
        void setRenderTarget();

        struct GraphNodes
        {
            eastl::vector<dabfg::NodeHandle> m_frameGraphNodes;

            bool m_isEnabled = true;

            void resetState();

            void setEnabled(bool enabled);
            void disableNodesWeak();
            void addNode(dabfg::NodeHandle&&);
        };

        async::Task<> setEnabledRenderStages(::nau::TypedFlag<NauRenderStage> stages, bool enabled);

        GraphNodes m_gBufferNodes;
        GraphNodes m_outlineNodes;
        GraphNodes m_environmentNodes;
        GraphNodes m_nauGuiNodes;
        GraphNodes m_postFxNodes;
        GraphNodes m_uidNodes;
        GraphNodes m_debugNodes;

        dabfg::NodeHandle m_csmNode;

#if defined(NAU_ENABLE_MGPU)
        // Mgpu path: the second GPU draws Nau's shadow cascades (the MGPU-ShadowMap scheme of Multi-GPU-Render-DX12).
        // The CSM node hands the casters of every cascade to the second GPU without waiting and copies the newest
        // finished atlas into Nau's atlas; gbuffer_resolve then uses the cascade matrices that atlas was drawn with.
        // With one GPU, until the first atlas has arrived, or with the path switched off
        // (nau::mgpu::bridge::setEnabled), Nau draws the cascades itself.
        struct MgpuShadowsState
        {
            ~MgpuShadowsState();

            // The second GPU is tried once per enable: a failed initialization is not retried every frame.
            bool initAttempted = false;
            nau::mgpu::shadows::CascadeRenderer renderer;

            // Shadow-caster meshes registered on the second GPU, by the Nau buffers they were read from. The sizes
            // catch most cases of a buffer address reused by another mesh.
            struct MeshRecord
            {
                uint64_t key = 0;
                int positionsSize = 0;
                int indicesSize = 0;
            };
            eastl::map<eastl::pair<const Sbuffer*, const Sbuffer*>, MeshRecord> meshes;
            uint64_t nextMeshKey = 1;

            // Alpha masks of masked casters on the second GPU, by the texture they come from (key 0: not registered).
            eastl::map<const void*, uint64_t> masks;
            uint64_t nextMaskKey = 1;

            uint64_t meshGeneration = 0; // nau::mgpu::bridge::getMeshGeneration() the meshes were registered in
            bool vsyncApplied = true; // the driver starts with vsync

            // Shader constants of the frames whose atlases are still on the second GPU, by FrameDesc::userToken.
            struct PendingFrame
            {
                uint64_t token = 0;
                uint64_t frame = 0;
                nau::csm::CascadeShadows::ShaderSnapshot snapshot;
                nau::render::OmniShadows::Snapshot omniSnapshot;  // point light shadows only (m_mgpuOmniShadows)
            };
            eastl::vector<PendingFrame> pendingFrames;
            uint64_t frame = 0;

            // Nau's atlas holds an atlas of the second GPU, drawn with atlasSnapshot for Nau frame atlasFrame.
            bool hasAtlas = false;
            nau::csm::CascadeShadows::ShaderSnapshot atlasSnapshot;
            nau::render::OmniShadows::Snapshot omniAtlasSnapshot;  // point light shadows only (m_mgpuOmniShadows)
            uint64_t atlasFrame = 0;

            // A slot handed to Nau is released once the Nau frame with its copy has finished on the GPU
            // (DRV3D_COMMAND_REGISTER_FRAME_COMPLETION_EVENT).
            bool releaseEventsCreated = false;
            eastl::array<os_event_t, nau::mgpu::shadows::SLOT_COUNT> releaseEvents = {};
            eastl::array<bool, nau::mgpu::shadows::SLOT_COUNT> releasePending = {};

            // Description of the frame for the second GPU, rebuilt every frame (kept to reuse the memory).
            eastl::vector<nau::mgpu::shadows::CascadeDesc> cascades;
            eastl::vector<nau::mgpu::shadows::DrawDesc> draws;
            eastl::vector<float> matrices;
            eastl::vector<eastl::pair<const void*, uint32_t>> bones; // bone arrays already in matrices, first matrix
            uint32_t skippedCasters = 0; // casters Nau draws but the second GPU cannot (mesh not registered, no bones)

            uint64_t reportedFailures = 0;
            uint64_t copyFailures = 0; // atlases the driver did not copy into Nau's atlas
            bool publishedHasAtlas = false;
            uint32_t atlasesSinceReport = 0;
            float atlasesPerSecond = 0;
            uint32_t framesSinceReport = 0;
            double cpuSecondsSinceReport = 0; // time the CSM node spends in renderMgpuShadows
            std::chrono::steady_clock::time_point lastReport;

            // Nau's render frame rate, counted in every mode (ImGui's frame rate is the one of the update loop).
            uint32_t renderFrames = 0;
            float renderFramesPerSecond = 0;
            std::chrono::steady_clock::time_point renderFramesStart;

            // Stage 0 of the MGPU optimisation: where the time of a frame goes (m_mgpuShadows only). Sums since the last
            // report, which turns them into averages (nau::mgpu::bridge::Status::Timings).
            struct Timings
            {
                GpuTimer gpuFrame; // Nau's frame graph on the scene GPU
                GpuTimer gpuNauCascades; // Nau's own cascade pass (MGPU off, or no atlas of the second GPU yet)
                GpuTimer gpuAtlasCopy; // an atlas of the second GPU into Nau's atlas
                std::chrono::steady_clock::time_point renderStart;
                uint32_t frames = 0;
                double renderSeconds = 0; // the window's views and the frame graph on the render thread, without present
                uint32_t listFrames = 0;
                eastl::array<double, nau::RenderScene::ViewsStats::KindCount> listSeconds = {};
                eastl::array<uint64_t, nau::RenderScene::ViewsStats::KindCount> listInstances = {};
                eastl::array<uint64_t, nau::csm::CascadeShadows::MAX_CASCADES> cascadeInstances = {};
                double collectSeconds = 0; // the casters for the second GPU
                double submitSeconds = 0; // recording and submitting an atlas of the second GPU
                // Sums of the renderer at the last report: the GPU times of the second GPU are differences.
                double drawGpuMs = 0;
                double sharedCopyGpuMs = 0;
                uint64_t drawGpuSamples = 0;
                double transferGpuMs = 0;
                uint64_t transferGpuSamples = 0;
                nau::mgpu::bridge::Status::Timings published;
            };
            Timings timings;

            // This frame's cascade lists were skipped (renderOptimizationsEnabled): the second GPU is busy and Nau keeps the
            // atlas it has. renderMgpuShadows submits nothing then; Nau rebuilds them if it draws the cascades after all.
            bool cascadeListsSkipped = false;
            uint32_t skippedListFrames = 0; // since the last report
            uint32_t listFallbacks = 0;
        };

        // Log line and status update for the UI (atlases per second, latency, failures).
        static constexpr float MGPU_REPORT_PERIOD_SECONDS = 2.0f;

        // Called by the CSM node after prepareShadowCascades. Returns true when Nau's atlas holds an atlas of the second
        // GPU for this frame: Nau then does not draw the cascades, and gbuffer_resolve uses m_mgpuShadows.atlasSnapshot.
        bool renderMgpuShadows();
        // Averages of the timings since the last report: the status and a log line.
        void reportMgpuTimings();
        // The cascade lists were skipped this frame but Nau draws the cascades: builds them now.
        void rebuildSkippedCascadeLists();
        nau::math::Vector3 m_viewerPosition = nau::math::Vector3::zero();
        void releaseMgpuShadows(MgpuShadowsState& state);
        // Releases the slots whose copy has executed; returns true when no slot is waiting any more.
        bool releaseMgpuShadowSlots(MgpuShadowsState& state);
        void collectMgpuShadowCasters(int numCascades);
        void appendMgpuViewCasters(MgpuShadowsState& state, nau::RenderView& view, uint32_t cascade);
        uint64_t registerMgpuShadowMesh(MgpuShadowsState& state, const nau::RenderEntity& entity);
        uint64_t registerMgpuShadowMask(MgpuShadowsState& state, const nau::RenderEntity& entity);
        void publishMgpuStatus() const;

        MgpuShadowsState m_mgpuShadows;

        // Point light shadow maps drawn by the second GPU (the same machinery as m_mgpuShadows, own renderer and atlas).
        // Returns true when the atlas of Nau holds an atlas of the second GPU; m_omniBound then describes it.
        bool renderMgpuOmniShadows(const eastl::vector<render::OmniShadowCaster>& casters);
        void collectMgpuOmniShadowCasters(const eastl::vector<render::OmniShadowCaster>& casters);
        MgpuShadowsState m_mgpuOmniShadows;
#endif

        eastl::map<NauRenderStage, GraphNodes&> m_graphStages = {
            {    NauRenderStage::GBufferStage,     m_gBufferNodes},
            {    NauRenderStage::OutlineStage,     m_outlineNodes},
            {NauRenderStage::EnvironmentStage, m_environmentNodes},
            {     NauRenderStage::NauGUIStage,      m_nauGuiNodes},
            {     NauRenderStage::PostFXStage,      m_postFxNodes},
            {        NauRenderStage::UIDStage,         m_uidNodes},
            {      NauRenderStage::DebugStage,       m_debugNodes}
        };

        std::optional<int32_t> m_activeCamera;

        nau::Uid m_world = nau::NullUid;
        eastl::shared_ptr<GraphicsScene> m_graphicsScene = nullptr;

        eastl::unique_ptr<render::PostFxRenderer> m_postFXRenderer;
        MaterialAssetView::Ptr m_depthOfFieldMaterial;
        bool m_depthOfFieldEnabled = false;
        bool m_depthOfFieldDebugEnabled = false;
        bool m_depthOfFieldToggleWasDown = false;
        bool m_depthOfFieldDebugToggleWasDown = false;
        float m_depthOfFieldFocusDistance = 25.0f;
        float m_depthOfFieldFocusRange = 6.0f;
        float m_depthOfFieldMaxCoC = 3.4f;
        float m_depthOfFieldFilterRadius = 3.0f;
        float m_depthOfFieldTaps = 32.0f;
        bool m_dofMultiGpuUsed = false;
#if defined(NAU_ENABLE_MGPU)
        nau::mgpu::bridge::DofBridge m_dofBridge;
        bool m_dofMultiGpuEnabled = true;
        bool m_dofMultiGpuToggleWasDown = false;
        bool m_dofMultiGpuReported = false;
#endif
        eastl::unique_ptr<render::EnvironmentRenderer> m_environmentRenderer;

        void* m_windowHandle;
        SWAPID m_swapchain;

        eastl::string m_name;
        eastl::string m_resolutionName;
        eastl::string m_displayName;
        WeakPtr<async::Executor> m_preRenderWorkQueueRef;

        eastl::unique_ptr<render::DeferredRenderTarget> m_gBuffer;

        nau::threading::SpinLock m_readWriteMutex;
        nau::threading::SpinLock m_resizeMutex;

        struct VieportObjectRequest
        {
            int32_t viewportX;
            int32_t viewportY;
            nau::async::TaskSource<nau::Uid> promise;
        };

        struct PixelData
        {
            nau::Uid uid;
            float depth;
        };

        eastl::vector<VieportObjectRequest> m_viewportRequests;
        MaterialAssetView::Ptr m_pixelDataExtractionMaterial;

        MaterialAssetView::Ptr m_gridMaterial;
        bool m_drawViewportGrid = false;

        std::atomic<float> m_outlineWidth;
        std::atomic<math::Color4> m_outlineColor;
        eastl::unique_ptr<render::PostFxRenderer> m_outlineRenderer;

        eastl::shared_ptr<nau::csm::CascadeShadows> m_csm;

        // Shadow maps of point lights (render/omniShadows.h). renderOmniShadows is called by the CSM node (draws the faces, or
        // takes the atlas of the second GPU), bindOmniShadows by gbuffer_resolve right before the point light pass.
        eastl::unique_ptr<render::OmniShadows> m_omniShadows;
        render::OmniShadows::Snapshot m_omniBound;  // constants of the atlas that the point light pass samples
        void renderOmniShadows();
        void bindOmniShadows();
        
        int32_t m_width;
        int32_t m_height;

        static constexpr int REQUEST_RESIZE = 5; // Wait 5 frames before actual
        static constexpr int PERFORM_RESIZE = 0;
        static constexpr int NO_RESIZE_REQUESTED = -1;

        // Frame count before resize will be done.
        // Resets each time when resize requested.
        int m_resizeFrameCounter = NO_RESIZE_REQUESTED;

        friend class nau::GraphicsImpl;
    };

}  // namespace nau::render
