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
#include "nau/3d/dag_resPtr.h"
#include "nau/mgpu/bridge/mgpu_bridge.h"
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

        virtual async::Task<nau::Uid> requestUidByCoords(int32_t viewportX, int32_t viewportY) override;

        void setOutlineWidth(float newWidth) override;
        void setOutlineColor(const math::Color4&) override;

        virtual async::Task<> enableRenderStages(::nau::TypedFlag<NauRenderStage> stages) override;
        virtual async::Task<> disableRenderStages(::nau::TypedFlag<NauRenderStage> stages) override;

        void setRenderScene(eastl::shared_ptr<GraphicsScene> gScene);

    private:
        void createGBufferNodes();
        void createOutlineNodes();

        // Registers the "mgpu_transfer" node. Without NAU_ENABLE_MGPU it registers nothing.
        void createMgpuNodes();

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
        // Mgpu transfer path: a second GPU produces a test image, it travels through a shared heap to the Mgpu
        // device on Nau's adapter and is written into a Nau texture. Switch: MGPU_TRANSFER_ENABLED in mgpu_nodes.cpp.
        struct MgpuTransferState
        {
            // The bridge is tried once per enable: a failed initialization is not retried every frame.
            bool initAttempted = false;
            uint32_t frameCounter = 0;
            nau::mgpu::bridge::TransferBridge bridge;
            TexPtr stagingTexture; // CPU-writable (system memory) copy of the received image
            TexPtr targetTexture; // GPU texture that receives the image from the staging texture
        };

        static constexpr uint32_t MGPU_TRANSFER_PERIOD_FRAMES = 60;

        void updateMgpuTransfer();
        void releaseMgpuTransfer();

        MgpuTransferState m_mgpuTransfer;
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
