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
#include "../graphics_settings.h"

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
        void beginFrameTiming();
        void endFrameTiming();
        void recordFrameTiming() const;
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

        void initializeMgpu();

        static int sparseShadowFrames()
        {
            return getGraphicsSettings().shadowSparseFrames;
        }
        void buildCascadeLists(const eastl::function<bool(int)>& draw);
        nau::math::Vector3 m_viewerPosition = nau::math::Vector3::zero();
        nau::math::Vector3 m_sparseLightDir = nau::math::Vector3::zero();
        int m_sparseNumCascades = -1;

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
        struct MgpuShadowsState
        {
            ~MgpuShadowsState();

            bool initAttempted = false;
            nau::mgpu::shadows::CascadeRenderer renderer;

            struct MeshRecord
            {
                uint64_t key = 0;
                int positionsSize = 0;
                int indicesSize = 0;
            };
            eastl::map<eastl::pair<const Sbuffer*, const Sbuffer*>, MeshRecord> meshes;
            uint64_t nextMeshKey = 1;

            eastl::map<const void*, uint64_t> masks;
            uint64_t nextMaskKey = 1;

            uint64_t meshGeneration = 0;
            bool vsyncApplied = true;

            struct PendingFrame
            {
                uint64_t token = 0;
                uint64_t frame = 0;
                nau::csm::CascadeShadows::ShaderSnapshot snapshot;
                nau::render::OmniShadows::Snapshot omniSnapshot;
                eastl::array<nau::math::Vector3, nau::csm::CascadeShadows::MAX_CASCADES> viewPos;
                std::chrono::steady_clock::time_point submitted;
            };
            eastl::vector<PendingFrame> pendingFrames;
            uint64_t frame = 0;

            bool hasAtlas = false;
            nau::csm::CascadeShadows::ShaderSnapshot atlasSnapshot;
            eastl::array<nau::math::Vector3, nau::csm::CascadeShadows::MAX_CASCADES> atlasViewPos;
            int nearCascades = 0;
            nau::csm::CascadeShadows::ShaderSnapshot resolveSnapshot;

            eastl::array<bool, nau::csm::CascadeShadows::MAX_CASCADES> redrawPending = {};
            bool fullRedraw = true;
            nau::csm::CascadeShadows::AtlasInfo redrawAtlas;
            int redrawNumCascades = -1;
            bool secondGpuCascadesInAtlas = false;

            std::chrono::steady_clock::time_point nextSubmit;

            bool stalled = false;
            uint32_t failedSubmitsInRow = 0;

            struct AutoSwitch
            {
                bool suspended = false;
                bool probing = false;
                int probePeriods = 0;
                float framesPerSecond = 0;
                float otherFramesPerSecond = 0;
                float intervalSeconds = 0;
                std::chrono::steady_clock::time_point nextProbe;
                uint64_t meshGeneration = 0;
            };
            AutoSwitch autoSwitch;
            nau::render::OmniShadows::Snapshot omniAtlasSnapshot;
            uint64_t atlasFrame = 0;

            bool releaseEventsCreated = false;
            eastl::array<os_event_t, nau::mgpu::shadows::SLOT_COUNT> releaseEvents = {};
            eastl::array<bool, nau::mgpu::shadows::SLOT_COUNT> releasePending = {};

            eastl::vector<nau::mgpu::shadows::CascadeDesc> cascades;
            eastl::vector<nau::mgpu::shadows::DrawDesc> draws;
            eastl::vector<float> matrices;
            eastl::vector<eastl::pair<const void*, uint32_t>> bones;
            uint32_t skippedCasters = 0;

            uint64_t reportedFailures = 0;
            uint64_t copyFailures = 0;
            bool publishedHasAtlas = false;
            uint32_t atlasesSinceReport = 0;
            float atlasesPerSecond = 0;
            uint32_t framesSinceReport = 0;
            double cpuSecondsSinceReport = 0;
            std::chrono::steady_clock::time_point lastReport;

            uint32_t renderFrames = 0;
            float renderFramesPerSecond = 0;
            std::chrono::steady_clock::time_point renderFramesStart;

            struct Timings
            {
                GpuTimer gpuFrame;
                GpuTimer gpuNauCascades;
                GpuTimer gpuAtlasCopy;
                std::chrono::steady_clock::time_point renderStart;
                uint32_t frames = 0;
                double renderSeconds = 0;
                uint32_t listFrames = 0;
                eastl::array<double, nau::RenderScene::ViewsStats::KindCount> listSeconds = {};
                eastl::array<uint64_t, nau::RenderScene::ViewsStats::KindCount> listInstances = {};
                eastl::array<uint64_t, nau::csm::CascadeShadows::MAX_CASCADES> cascadeInstances = {};
                eastl::array<uint64_t, nau::csm::CascadeShadows::MAX_CASCADES> cascadeTriangles = {};
                uint64_t nauCascadeDraws = 0;
                uint64_t secondGpuCascadeDraws = 0;
                uint32_t submits = 0;
                uint32_t unchangedFrames = 0;
                double collectSeconds = 0;
                double submitSeconds = 0;
                double drawGpuMs = 0;
                double sharedCopyGpuMs = 0;
                uint64_t drawGpuSamples = 0;
                double transferGpuMs = 0;
                uint64_t transferGpuSamples = 0;
                nau::mgpu::bridge::Status::Timings published;
            };
            Timings timings;

            bool cascadeListsSkipped = false;
            uint32_t skippedListFrames = 0;
            uint32_t listFallbacks = 0;
        };

        static constexpr float MGPU_REPORT_PERIOD_SECONDS = 2.0f;

        static constexpr float MGPU_STALL_SECONDS = 1.0f;
        static constexpr uint64_t MGPU_STALL_FRAMES = 10;
        static constexpr uint32_t MGPU_FAILED_SUBMITS_LIMIT = 3;
        bool checkMgpuStall(MgpuShadowsState& state, const char* what);
        static void noteMgpuSubmit(MgpuShadowsState& state, uint64_t failedBefore, bool submitted);

        bool renderMgpuShadows();
        static bool mgpuSubmitDue(const MgpuShadowsState& state);
        static void mgpuSubmitted(MgpuShadowsState& state);
        void updateMgpuAutoSwitch();
        bool suspendMgpuRenderer(MgpuShadowsState& state);
        void renderMgpuNearCascades();
        void updateMgpuResolveSnapshot();
        void reportMgpuTimings();
        void releaseMgpuShadows(MgpuShadowsState& state);
        bool releaseMgpuShadowSlots(MgpuShadowsState& state);
        void collectMgpuShadowCasters(int firstCascade, int numCascades);
        void appendMgpuViewCasters(MgpuShadowsState& state, nau::RenderView& view, uint32_t cascade);
        uint64_t registerMgpuShadowMesh(MgpuShadowsState& state, const nau::RenderEntity& entity);
        uint64_t registerMgpuShadowMask(MgpuShadowsState& state, const nau::RenderEntity& entity);
        void publishMgpuStatus() const;

        MgpuShadowsState m_mgpuShadows;

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

        eastl::unique_ptr<render::OmniShadows> m_omniShadows;
        render::OmniShadows::Snapshot m_omniBound;
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
