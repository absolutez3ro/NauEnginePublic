// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <cstring>

#include "nau/3d/dag_drv3d.h"
#include "nau/diag/logging.h"
#include "render/daBfg/bfg.h"
#include "render_window_impl.h"

namespace nau::render
{
#if defined(NAU_ENABLE_MGPU)
    // Initial state of the Mgpu path of the render pipeline: the second GPU draws an image in parallel with Nau's
    // frame, it travels through a shared heap to the Mgpu device on Nau's adapter, is written into a Nau texture and
    // shown instead of the scene every other MGPU_ALTERNATE_PERIOD_FRAMES frames. The value is applied once, when the
    // first render graph is created; nau::mgpu::bridge::setEnabled() switches the path at run time (SceneBaseSample:
    // key M).
    // It only has an effect in a build with NAU_ENABLE_MGPU (preset win_vs2022_x64_mgpu) on a machine with two distinct
    // GPUs; with one GPU the bridge reports that it is unavailable and only the scene is shown.
    constexpr bool MGPU_TRANSFER_ENABLED = false; // Test pattern is independent of the DOF bridge.

    namespace
    {
        void copyText(char* destination, const size_t capacity, const char* source)
        {
            std::strncpy(destination, source != nullptr ? source : "", capacity - 1);
            destination[capacity - 1] = '\0';
        }
    }
#endif

    void RenderWindowImpl::createMgpuNodes()
    {
#if defined(NAU_ENABLE_MGPU)
        static const bool switchApplied = (nau::mgpu::bridge::setEnabled(MGPU_TRANSFER_ENABLED), true);
        static_cast<void>(switchApplied);

        // The node is kept in its own handle, outside the debug stage: GraphicsImpl::createRenderWindow disables
        // that stage right after the render graph is created. While the Mgpu path is switched off the node returns
        // right away (see updateMgpuTransfer).
        m_mgpuNode = dabfg::register_node(nau::utils::format("{}_{}", "mgpu_transfer", m_swapchain).c_str(), DABFG_PP_NODE_SRC,
                                          [this](dabfg::Registry registry)
        {
            // post_fx_nodes is the last pass that writes the back buffer in SceneBaseSample.
            registry.orderMeAfter(nau::utils::format("{}_{}", "post_fx_nodes", m_swapchain).c_str());
            registry.executionHas(dabfg::SideEffects::External);

            return [this]()
            {
                updateMgpuTransfer();
            };
        });
#endif
    }

#if defined(NAU_ENABLE_MGPU)

    void RenderWindowImpl::releaseMgpuTransfer()
    {
        m_mgpuTransfer.bridge.shutdown();
        m_mgpuTransfer.targetTexture.close();
        m_mgpuTransfer.stagingTexture.close();
        m_mgpuTransfer.initAttempted = false;
        m_mgpuTransfer.hasImage = false;
        m_mgpuTransfer.frameCounter = 0;
        m_mgpuTransfer.reportedFailures = 0;
        m_mgpuTransfer.imagesSinceReport = 0;
        m_mgpuTransfer.imagesPerSecond = 0;
    }

    void RenderWindowImpl::publishMgpuStatus() const
    {
        const auto& state = m_mgpuTransfer;
        const nau::mgpu::bridge::TransferStats& stats = state.bridge.stats();

        nau::mgpu::bridge::Status status;
        status.ready = state.bridge.isReady();
        copyText(status.primaryAdapter, sizeof(status.primaryAdapter), state.bridge.primaryAdapterName());
        copyText(status.secondaryAdapter, sizeof(status.secondaryAdapter), state.bridge.secondaryAdapterName());
        copyText(status.lastError, sizeof(status.lastError), state.bridge.lastError());
        status.imagesPerSecond = state.imagesPerSecond;
        status.latencyMs = stats.lastLatencyMs;
        status.imagesOk = stats.imagesOk;
        status.imagesFailed = stats.imagesFailed;
        nau::mgpu::bridge::publishStatus(status);
    }

    // The newest image of the second GPU -> Nau texture: through a system-memory texture, then a GPU copy.
    void RenderWindowImpl::uploadMgpuImage()
    {
        auto& state = m_mgpuTransfer;
        const nau::mgpu::bridge::TransferInfo& info = state.bridge.info();

        if (!state.targetTexture)
        {
            state.stagingTexture = dag::create_tex(nullptr, static_cast<int>(info.width), static_cast<int>(info.height),
                                                   TEXFMT_R8G8B8A8 | TEXCF_SYSMEM | TEXCF_DYNAMIC | TEXCF_READABLE | TEXCF_LINEAR_LAYOUT,
                                                   1, "mgpu_transfer_staging");
            state.targetTexture = dag::create_tex(nullptr, static_cast<int>(info.width), static_cast<int>(info.height),
                                                  TEXFMT_R8G8B8A8 | TEXCF_UPDATE_DESTINATION, 1, "mgpu_transfer_tex");
        }

        if (!state.stagingTexture || !state.targetTexture)
        {
            NAU_LOG_ERROR("mgpu transfer: could not create the Nau textures");
            return;
        }

        void* locked = nullptr;
        int stride = 0;
        if (!state.stagingTexture.get()->lockimg(&locked, stride, 0, TEXLOCK_WRITE | TEXLOCK_RAWDATA))
        {
            NAU_LOG_ERROR("mgpu transfer: could not lock the staging texture");
            return;
        }

        const auto* source = static_cast<const uint8_t*>(state.bridge.pixels());
        auto* destination = static_cast<uint8_t*>(locked);
        for (uint32_t y = 0; y < info.height; ++y)
        {
            std::memcpy(destination + static_cast<size_t>(y) * stride, source + static_cast<size_t>(y) * info.rowPitch, info.rowPitch);
        }
        state.stagingTexture.get()->unlockimg();

        state.targetTexture.get()->update(state.stagingTexture.get());
        state.hasImage = true;
    }

    void RenderWindowImpl::updateMgpuTransfer()
    {
        auto& state = m_mgpuTransfer;

        if (!nau::mgpu::bridge::isEnabled())
        {
            if (state.initAttempted)
            {
                releaseMgpuTransfer();
                publishMgpuStatus();
                NAU_LOG_INFO("mgpu transfer: switched off, only the scene is shown");
            }
            return;
        }

        if (!state.initAttempted)
        {
            state.initAttempted = true;
            state.lastReport = std::chrono::steady_clock::now();

            // The primary Mgpu device is the one on the adapter Nau renders on.
            if (state.bridge.initialize(d3d::get_device_name()))
            {
                NAU_LOG_INFO("mgpu transfer: ready, primary adapter '{}', secondary adapter '{}'",
                             state.bridge.primaryAdapterName(), state.bridge.secondaryAdapterName());
            }
            else
            {
                NAU_LOG_WARNING("mgpu transfer: not available, only the scene is shown: {}", state.bridge.lastError());
            }
            publishMgpuStatus();
        }

        if (!state.bridge.isReady())
        {
            return;
        }

        // Never waits: collects the images that have arrived and keeps the second GPU busy with the next one.
        const uint32_t frame = state.frameCounter++;
        if (state.bridge.pump(frame))
        {
            uploadMgpuImage();
            ++state.imagesSinceReport;
        }

        const nau::mgpu::bridge::TransferStats& stats = state.bridge.stats();
        if (stats.imagesFailed != state.reportedFailures)
        {
            NAU_LOG_ERROR("mgpu transfer: {} image(s) FAILED: {} (received {:016x}, produced on GPU2 {:016x})",
                          stats.imagesFailed - state.reportedFailures, state.bridge.lastError(),
                          state.bridge.info().checksum, state.bridge.info().expectedChecksum);
            state.reportedFailures = stats.imagesFailed;
        }

        // Scene frames leave the back buffer untouched; texture frames show the newest image of the second GPU over
        // the whole back buffer (scaled blit). Until the first image has arrived only the scene is shown.
        const bool textureFrame = (frame / MGPU_ALTERNATE_PERIOD_FRAMES) % 2 == 1;
        if (textureFrame && state.hasImage)
        {
            if (!d3d::stretch_rect(state.targetTexture.get(), d3d::get_back_buffer_rt(m_swapchain)))
            {
                NAU_LOG_ERROR("mgpu transfer: could not copy 'mgpu_transfer_tex' to the back buffer");
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - state.lastReport).count();
        if (elapsed >= MGPU_REPORT_PERIOD_SECONDS)
        {
            state.imagesPerSecond = static_cast<float>(state.imagesSinceReport) / elapsed;
            state.imagesSinceReport = 0;
            state.lastReport = now;
            publishMgpuStatus();

            NAU_LOG_INFO("mgpu transfer: {:.1f} images/s from '{}', latency {:.1f} ms, {} OK, {} failed",
                         state.imagesPerSecond, state.bridge.secondaryAdapterName(), stats.lastLatencyMs,
                         stats.imagesOk, stats.imagesFailed);
        }
    }

#endif
}  // namespace nau::render
