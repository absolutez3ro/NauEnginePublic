// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "render_window_impl.h"

#include <cstdio>

#include "nau/app/core_window_manager.h"
#include "nau/app/global_properties.h"
#include "nau/graphics/core_graphics.h"
#include "nau/input.h"
#include "nau/platform/windows/app/windows_window.h"

namespace nau::render
{
    namespace
    {
        async::Task<> setWindowTitle(void* handle, eastl::string title)
        {
            ASYNC_SWITCH_EXECUTOR(getServiceProvider().get<ICoreWindowManager>().getExecutor());
            HWND window = static_cast<HWND>(handle);
            if (IsWindow(window))
            {
                SetWindowTextA(window, title.c_str());
            }
        }
    }  // namespace

    void RenderWindowImpl::updateFrameStatistics()
    {
        if (!m_showFrameStatistics)
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_frameStatisticsStart == std::chrono::steady_clock::time_point{})
        {
            m_frameStatisticsStart = now;
            return;
        }
        ++m_frameStatisticsCount;
        const double milliseconds = std::chrono::duration<double, std::milli>(now - m_frameStatisticsStart).count();
        if (milliseconds < 1000.0)
        {
            return;
        }
        // Count render frames, including presentation pacing; these are not GPU pass timings.
        char title[128] = {};
        std::snprintf(title, sizeof(title), "NAU | %.1f FPS | %.2f ms/frame",
                      1000.0 * m_frameStatisticsCount / milliseconds, milliseconds / m_frameStatisticsCount);
        setWindowTitle(m_windowHandle, title).detach();
        m_frameStatisticsCount = 0;
        m_frameStatisticsStart = now;
    }

    void RenderWindowImpl::setWorld(nau::Uid worldUid)
    {
        lock_(m_readWriteMutex);
        m_world = worldUid;
    }

    nau::Uid RenderWindowImpl::getWorld()
    {
        lock_(m_readWriteMutex);
        return m_world;
    }

    async::Task<> RenderWindowImpl::requestViewportResize(int32_t width, int32_t height)
    {
        lock_(m_resizeMutex);
        m_width = width;
        m_height = height;
        m_resizeFrameCounter = REQUEST_RESIZE;
        co_return;
    }

    async::Task<> RenderWindowImpl::requestViewportResizeImmediate(int32_t width, int32_t height)
    {
        lock_(m_resizeMutex);
        m_width = width;
        m_height = height;
        m_resizeFrameCounter = PERFORM_RESIZE;

        co_return;
    }

    void RenderWindowImpl::resizeResolutions()
    {
        d3d::set_screen_size(m_width, m_height, m_swapchain);

        dabfg::set_resolution(m_resolutionName.c_str(), {m_width, m_height});
        dabfg::set_resolution(m_displayName.c_str(), {m_width, m_height});
        m_gBuffer->changeResolution(m_width, m_height);

        if (m_swapchain == DEFAULT_SWAPID)
        {
            nau::input::setScreenResolution(m_width, m_height);
        }
    }

    void RenderWindowImpl::getViewportSize(int32_t& width, int32_t& height)
    {
        lock_(m_readWriteMutex);

        width = m_width;
        height = m_height;
    }

    eastl::string_view RenderWindowImpl::getName() const
    {
        return m_name;
    }

    void RenderWindowImpl::setName(eastl::string_view name)
    {
        m_name = name;
        m_displayName = nau::utils::format("{}_view", name);
        m_resolutionName = nau::utils::format("{}_display", name);
    }

    int32_t RenderWindowImpl::getMainCameraIndex()
    {
        lock_(m_readWriteMutex);
        if (m_activeCamera)
        {
            return *m_activeCamera;
        }
        return -1;
    }

    void RenderWindowImpl::setMainCameraIndex(int32_t ind)
    {
        lock_(m_readWriteMutex);
        m_activeCamera = ind;
    }

    AmbientOcclusionMode RenderWindowImpl::getAmbientOcclusionMode() const
    {
        return m_ambientOcclusionMode.load(std::memory_order_relaxed);
    }

    void RenderWindowImpl::setAmbientOcclusionMode(AmbientOcclusionMode mode)
    {
        m_ambientOcclusionMode.store(mode, std::memory_order_relaxed);
    }

    void RenderWindowImpl::render()
    {
        updateFrameStatistics();
        for (auto& [_, nodeGroup] : m_graphStages)
        {
            nodeGroup.resetState();
        }

        for (auto nodeGroup :
             {&m_gBufferNodes,
              &m_nauGuiNodes,
              &m_postFxNodes,
              &m_uidNodes,
              &m_environmentNodes})
        {
            if (!m_graphicsScene)
            {
                nodeGroup->disableNodesWeak();
            }
        }

        if (nau::input::isKeyboardButtonHold(0, nau::input::Key::L))
        {
            m_csm->getNumCascadesToRender();
        }

        if(m_graphicsScene)
        {
            if(m_graphicsScene->hasMainCamera())
            {
                nau::math::Matrix4 vp = m_graphicsScene->getMainCamera().getProjMatrix() * m_graphicsScene->getMainCamera().getViewMatrix();
                for (auto& view : m_graphicsScene->getRenderScene()->getViews())
                {
                    if (view->containsTag(nau::RenderScene::Tags::shadowCascadeTag))
                    {
                        int cascade = (int)view->getUserData();
                        NAU_ASSERT(cascade < nau::csm::CascadeShadows::MAX_CASCADES);
                        view->updateFrustum(m_csm->getWorldRenderMatrix(cascade));
                    }
                    else
                    {
                        view->updateFrustum(vp);
                    }
                }
                m_graphicsScene->getRenderScene()->updateViews(vp);
            }

        }

        {
            lock_(m_resizeMutex);

            if (m_resizeFrameCounter == PERFORM_RESIZE)
            {
                resizeResolutions();
                m_resizeFrameCounter = NO_RESIZE_REQUESTED;
            }

            if (m_resizeFrameCounter != NO_RESIZE_REQUESTED)
            {
                m_resizeFrameCounter--;
            }
        }
    }

    void RenderWindowImpl::setRenderScene(eastl::shared_ptr<GraphicsScene> gScene)
    {
        m_graphicsScene = gScene;
    }

    async::Task<nau::Uid> RenderWindowImpl::requestUidByCoords(int32_t viewportX, int32_t viewportY)
    {
        using namespace nau::async;

        if (Executor::Ptr renderWorkQueue = m_preRenderWorkQueueRef ? m_preRenderWorkQueueRef.lock() : nullptr)
        {
            ASYNC_SWITCH_EXECUTOR(renderWorkQueue);
        }
        else
        {
            NAU_LOG_ERROR("Render work queue is not accessible");
            co_return NullUid;
        }

        Task<Uid> future = m_viewportRequests.emplace_back(viewportX, viewportY).promise.getTask();
        future.setContinueOnCapturedExecutor(false);

        Uid result = co_await future;
        co_return result;
    }

    void RenderWindowImpl::setWorkQueue(WorkQueue::Ptr workQueue)
    {
        m_preRenderWorkQueueRef = std::move(workQueue);
    }

    void RenderWindowImpl::setOutlineWidth(float newWidth)
    {
        m_outlineWidth = newWidth;
    }

    void RenderWindowImpl::setOutlineColor(const math::Color4& color)
    {
        m_outlineColor.store(color, std::memory_order_seq_cst);
    }

    async::Task<> RenderWindowImpl::setEnabledRenderStages(::nau::TypedFlag<NauRenderStage> stages, bool enabled)
    {
        if (async::Executor::Ptr renderWorkQueue = m_preRenderWorkQueueRef ? m_preRenderWorkQueueRef.lock() : nullptr)
        {
            ASYNC_SWITCH_EXECUTOR(renderWorkQueue);
        }
        else
        {
            NAU_LOG_ERROR("Render work queue is not accessible");
            co_return;
        }

        for (auto& [stage, nodes] : m_graphStages)
        {
            if (stages.has(stage))
            {
                nodes.setEnabled(enabled);
            }
        }
    }

    async::Task<> RenderWindowImpl::enableRenderStages(::nau::TypedFlag<NauRenderStage> stages)
    {
        co_await setEnabledRenderStages(stages, true);
    }

    async::Task<> RenderWindowImpl::disableRenderStages(::nau::TypedFlag<NauRenderStage> stages)
    {
        co_await setEnabledRenderStages(stages, false);
    }

    void RenderWindowImpl::setRenderTarget()
    {
        BaseTexture* backBuf = d3d::get_back_buffer_rt(m_swapchain);
        d3d::set_render_target();
        d3d::set_render_target(backBuf, 0);
    }

    void* RenderWindowImpl::getHwnd() const
    {
        return m_windowHandle;
    }

    void RenderWindowImpl::initialize(eastl::string_view name, SWAPID swapchain, void* hwnd)
    {
        setName(name);
        m_swapchain = swapchain;
        m_windowHandle = hwnd;
        m_showFrameStatistics = getServiceProvider().get<GlobalProperties>().getValue<bool>("render/showFrameStatistics").value_or(false);

        nau::csm::CascadeShadows::Settings csmSettings;
        csmSettings.cascadeWidth = 1024;
        csmSettings.splitsW = csmSettings.splitsH = 2;
        m_csm.reset(nau::csm::CascadeShadows::make(this, csmSettings));
    }

    SWAPID RenderWindowImpl::getSwapchain() const
    {
        return m_swapchain;
    }
    void RenderWindowImpl::GraphNodes::addNode(dabfg::NodeHandle&& node)
    {
        m_frameGraphNodes.emplace_back(std::move(node));
    }

    void RenderWindowImpl::GraphNodes::setEnabled(bool enabled)
    {
        m_isEnabled = enabled;
        resetState();
    }

    void RenderWindowImpl::GraphNodes::disableNodesWeak()
    {
        for (auto& nodeId : m_frameGraphNodes)
        {
            dabfg::set_node_enabled(nodeId, false);
        }
    }

    void RenderWindowImpl::GraphNodes::resetState()
    {
        for (auto& nodeId : m_frameGraphNodes)
        {
            dabfg::set_node_enabled(nodeId, m_isEnabled);
        }
    }



    void RenderWindowImpl::renderCascadeShadowDepth(int cascade, const nau::math::Vector2 &znzf)
    {
        for (auto& view : m_graphicsScene->getRenderScene()->getViews())
        {
            if (view->containsTag(nau::RenderScene::Tags::shadowCascadeTag) && cascade == int(view->getUserData()))
            {
                view->renderZPrepass(m_csm->getWorldRenderMatrix(cascade), m_graphicsScene->getRenderScene()->getZPrepassMaterial().get());
                return;
            }
        }
    }

    eastl::shared_ptr<nau::csm::CascadeShadows> RenderWindowImpl::getCSM()
    {
        return m_csm;
    }

    void RenderWindowImpl::setDrawViewportGrid(bool isDrawGrid)
    {
        m_drawViewportGrid = isDrawGrid;
    }

    bool RenderWindowImpl::getDrawViewportGrid()
    {
        return m_drawViewportGrid;
    }

    void RenderWindowImpl::getCascadeShadowAnchorPoint(float cascade_from, nau::math::Vector3& out_anchor)
    {
        if (!m_graphicsScene->hasMainCamera())
        {
            return;
        }

        nau::CameraNode& cam = m_graphicsScene->getMainCamera();
        out_anchor = -cam.worldPosition;
    }

    void RenderWindowImpl::getCascadeShadowSparseUpdateParams(int cascade_no, const nau::math::NauFrustum& cascade_frustum, float& out_min_sparse_dist, int& out_min_sparse_frame)
    {
        out_min_sparse_dist = 100000;
        out_min_sparse_frame = -1000;
    }

}  // namespace nau::render
