// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "render_window_impl.h"

#include <cstdio>
#include <iomanip>
#include <locale>

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
        m_ambientOcclusionWaitMs.store(m_ambientOcclusion ? m_ambientOcclusion->getWaitMilliseconds() : -1.f, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        recordFrameMeasurements(now);
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
        m_frameRate.store(static_cast<float>(1000.0 * m_frameStatisticsCount / milliseconds), std::memory_order_relaxed);
        char wait[64] = {};
        const char* waitLabel = m_ambientOcclusion ? m_ambientOcclusion->getWaitLabel() : "n/a";
        if (waitLabel) std::snprintf(wait, sizeof(wait), "%s", waitLabel);
        else std::snprintf(wait, sizeof(wait), "%.3f ms", m_ambientOcclusion->getWaitMilliseconds());
        char title[384] = {};
        std::snprintf(title, sizeof(title), "NAU | %.1f FPS | %.2f ms/frame | GPU0 wait: %s | %s | %s | F8 GPU / F9 preset",
                      1000.0 * m_frameStatisticsCount / milliseconds, milliseconds / m_frameStatisticsCount,
                      wait,
                      m_ambientOcclusion ? m_ambientOcclusion->getExecutionLabel() : "AO disabled",
                      m_ambientOcclusion ? m_ambientOcclusion->getVariantLabel() : "");
        if (m_showFrameStatistics) setWindowTitle(m_windowHandle, title).detach();
        m_frameStatisticsCount = 0;
        m_frameStatisticsStart = now;
    }

    void RenderWindowImpl::recordFrameMeasurements(std::chrono::steady_clock::time_point now)
    {
        auto& properties = getServiceProvider().get<GlobalProperties>();
        if (!m_measurementsInitialized)
        {
            m_measurementsInitialized = true;
            m_measurementsOrigin = m_measurementsPrevious = now;
            if (!properties.getValue<bool>("render/measurements/enabled").value_or(false)) return;
            const auto path = properties.getValue<eastl::string>("render/measurements/csvPath").value_or("");
            if (path.empty()) return;
            m_measurementsCsv.open(path.c_str(), std::ios::out | std::ios::trunc);
            if (!m_measurementsCsv) { NAU_LOG_ERROR("Measurements: cannot open {}", path); return; }
            m_measurementsCsv.imbue(std::locale::classic());
            m_measurementsCsv << "frame,elapsed_ms,frame_ms,phase,width,height,requested_gpu,actual_gpu,gpu1_available,wait_enabled,cpu_serialize,ao_valid,ao_source_frame,ao_age_frames,timing_frame,overlap_gpu_ms,wait_interval_ms,ready_at_record_cpu,gpu1_timing_frame,gpu1_raw_ms,gpu1_total_ms,chain_frame,chain_observed_ms,load_mask,submit_only_handoffs,handoff_releases,handoff_waits,handoff_frame_flushes,handoff_cpu_ms,handoff_slot_waits,handoff_slot_wait_ms,frame_slot_waits,frame_slot_wait_ms\n";
            m_measurementsCsv << std::fixed << std::setprecision(6);
            NAU_LOG_INFO("Measurements: CSV {} (frame intervals are CPU render/present cadence; chain latency is a frame-polled upper bound)", path);
        }
        if (!m_measurementsCsv.is_open() || !m_measurementsCsv) return;
        const double dt = std::chrono::duration<double, std::milli>(now - m_measurementsPrevious).count();
        m_measurementsPrevious = now;
        if (!m_ambientOcclusion || !m_ambientOcclusion->getMeasurements().frame) return;
        const auto& m = m_ambientOcclusion->getMeasurements();
        const auto handoffs = d3d::mgpu::get_foreign_queue_measurements();
        const int64_t age = m.aoValid ? static_cast<int64_t>(m.frame - m.aoFrame) : -1;
        m_measurementsCsv << m.frame << ',' << std::chrono::duration<double, std::milli>(now - m_measurementsOrigin).count() << ',' << dt << ','
            << m.phase << ',' << m_gBuffer->getWidth() << ',' << m_gBuffer->getHeight() << ','
            << static_cast<uint32_t>(getAmbientOcclusionExecution()) << ',' << (m.gpu1 ? 1 : 0) << ',' << (d3d::is_multi_gpu_available() ? 1 : 0) << ','
            << (getAmbientOcclusionPipeline().waitForSecondary ? 1 : 0) << ',' << (properties.getValue<bool>("graphics/serializeRenderJobs").value_or(false) ? 1 : 0) << ','
            << (m.aoValid ? 1 : 0) << ',' << m.aoFrame << ',' << age << ',' << m.timingFrame << ',' << m.overlapMs << ',' << m.waitMs << ',' << m.readyAtConsumer << ','
            << m.gpu1Timing.frame << ',' << m.gpu1Timing.rawMilliseconds << ',' << m.gpu1Timing.totalMilliseconds << ',' << m.chainFrame << ',' << m.chainObservedMs << ','
            << m.loadMask << ',' << (handoffs.submitOnly ? 1 : 0) << ',' << handoffs.releases << ',' << handoffs.waits << ','
            << handoffs.frameFlushes << ',' << handoffs.cpuMilliseconds << ',' << handoffs.slotWaits << ',' << handoffs.slotWaitMilliseconds << ','
            << handoffs.frameSlotWaits << ',' << handoffs.frameSlotWaitMilliseconds << '\n';
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

    AmbientOcclusionExecution RenderWindowImpl::getAmbientOcclusionExecution() const
    {
        return m_ambientOcclusionExecution.load(std::memory_order_relaxed);
    }

    void RenderWindowImpl::setAmbientOcclusionExecution(AmbientOcclusionExecution execution)
    {
        m_ambientOcclusionExecution.store(execution, std::memory_order_relaxed);
    }

    AmbientOcclusionVariant RenderWindowImpl::getAmbientOcclusionVariant() const
    {
        return m_ambientOcclusionVariant.load(std::memory_order_relaxed);
    }

    void RenderWindowImpl::setAmbientOcclusionVariant(AmbientOcclusionVariant variant)
    {
        AmbientOcclusionPipeline pipeline;
        pipeline.waitForSecondary = getAmbientOcclusionPipeline().waitForSecondary;
        if (variant == AmbientOcclusionVariant::bogdanov)
        {
            pipeline.inputs = AmbientOcclusionInputs::full;
            pipeline.algorithm = variant;
            pipeline.blur = AmbientOcclusionBlur::gaussian;
            pipeline.upsample = AmbientOcclusionUpsample::bilinear;
        }
        setAmbientOcclusionPipeline(pipeline);
        m_ambientOcclusionVariant.store(variant, std::memory_order_relaxed);
    }

    AmbientOcclusionPipeline RenderWindowImpl::getAmbientOcclusionPipeline() const
    {
        std::lock_guard lock(m_ambientOcclusionMutex);
        return m_ambientOcclusionPipeline;
    }

    void RenderWindowImpl::setAmbientOcclusionPipeline(const AmbientOcclusionPipeline& pipeline)
    {
        std::lock_guard lock(m_ambientOcclusionMutex);
        m_ambientOcclusionPipeline = pipeline;
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
        if (m_ambientOcclusion)
        {
            const AmbientOcclusion::Settings settings{getAmbientOcclusionMode(), getAmbientOcclusionExecution(), getAmbientOcclusionPipeline()};
            m_ambientOcclusion->beginFrame(m_gBuffer->getWidth(), m_gBuffer->getHeight(), settings);
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
