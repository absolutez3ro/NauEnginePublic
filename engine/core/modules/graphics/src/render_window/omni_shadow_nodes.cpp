// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <algorithm>
#include <cstring>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/diag/logging.h"
#include "render_window_impl.h"

// Shadow maps of point lights: the faces are drawn by Nau (one GPU) or by the second GPU (Mgpu path), see render/omniShadows.h.
namespace nau::render
{
    void RenderWindowImpl::renderOmniShadows()
    {
        if (!m_graphicsScene)
        {
            return;
        }

        const eastl::vector<OmniShadowCaster> casters = m_graphicsScene->getOmniShadowCasters();

        if (!m_omniShadows)
        {
            m_omniShadows = eastl::make_unique<OmniShadows>();
        }

        bool atlasFromSecondGpu = false;
#if defined(NAU_ENABLE_MGPU)
        // Also called without casters: it releases the slots handed out earlier and shuts the path down when it is switched off.
        atlasFromSecondGpu = renderMgpuOmniShadows(casters);
#endif

        if (casters.empty())
        {
            m_omniBound = {};
            return;
        }

        if (!atlasFromSecondGpu)
        {
            m_omniShadows->renderFaces(casters, *m_graphicsScene->getRenderScene());
            m_omniBound = m_omniShadows->makeSnapshot(casters);
        }
    }

    void RenderWindowImpl::bindOmniShadows()
    {
        if (!m_omniShadows || !m_graphicsScene)
        {
            return;
        }

        m_omniShadows->uploadConstants(m_omniBound);
        m_graphicsScene->setOmniShadowBinding(m_omniBound.lightIds,
                                              m_omniBound.count,
                                              m_omniShadows->getConstantBuffer(),
                                              m_omniShadows->getAtlas(),
                                              m_omniShadows->getSampler());
    }

#if defined(NAU_ENABLE_MGPU)

    namespace
    {
        namespace shadows = nau::mgpu::shadows;

        // drv3d_DX12 (render_state.h): D3D12 DepthBias = zBias / MINIMUM_REPRESENTABLE_D16, truncated to INT.
        constexpr double DX12_MINIMUM_REPRESENTABLE_D16 = 2e-5;

        constexpr float MGPU_OMNI_REPORT_PERIOD_SECONDS = 2.0f;
    }

    // The faces of the point light shadow maps, as a frame description for the second GPU: every face is a "cascade" of the
    // atlas (viewport, view-projection, depth bias), the casters of a face are the draws of its omniView_<slot>_<face>.
    void RenderWindowImpl::collectMgpuOmniShadowCasters(const eastl::vector<OmniShadowCaster>& casters)
    {
        auto& state = m_mgpuOmniShadows;
        state.cascades.clear();
        state.draws.clear();
        state.matrices.clear();
        state.bones.clear();
        state.skippedCasters = 0;

        float constBias = 0;
        float slopeBias = 0;
        OmniShadows::getDepthBias(constBias, slopeBias);

        const uint32_t slots = static_cast<uint32_t>(std::min<size_t>(casters.size(), omni_shadow::MAX_LIGHTS));
        for (uint32_t slot = 0; slot < slots; ++slot)
        {
            for (uint32_t face = 0; face < omni_shadow::FACES; ++face)
            {
                shadows::CascadeDesc& cascade = state.cascades.emplace_back();
                std::memcpy(cascade.viewProj, &casters[slot].faceViewProj[face], sizeof(cascade.viewProj));

                OmniShadows::getFaceViewport(slot, face, cascade.viewport[0], cascade.viewport[1], cascade.viewport[2]);
                cascade.viewport[3] = cascade.viewport[2];
                cascade.depthBias = static_cast<int32_t>(constBias / DX12_MINIMUM_REPRESENTABLE_D16);
                cascade.slopeScaledDepthBias = slopeBias;
                cascade.depthClip = true;
            }
        }

        const uint32_t faceCount = static_cast<uint32_t>(state.cascades.size());
        for (auto& view : m_graphicsScene->getRenderScene()->getViews())
        {
            if (!view->containsTag(nau::RenderScene::Tags::shadowOmniTag))
            {
                continue;
            }
            const uint32_t index = static_cast<uint32_t>(reinterpret_cast<intptr_t>(view->getUserData()));
            if (index >= faceCount)
            {
                continue;
            }

            appendMgpuViewCasters(state, *view, index);
        }
    }

    bool RenderWindowImpl::renderMgpuOmniShadows(const eastl::vector<OmniShadowCaster>& casters)
    {
        auto& state = m_mgpuOmniShadows;
        ++state.frame;

        if (!nau::mgpu::bridge::isEnabled())
        {
            state.hasAtlas = false;
            // Shut down once Nau's copies from the shared heaps have executed; until then nothing new is started.
            if (state.initAttempted && releaseMgpuShadowSlots(state))
            {
                releaseMgpuShadows(state);
                NAU_LOG_INFO("mgpu omni shadows: switched off, Nau draws the point light shadow maps");
            }
            return false;
        }

        if (casters.empty())
        {
            // Nothing to draw, but the slots handed to Nau are still released when their copy has executed.
            if (state.initAttempted)
            {
                releaseMgpuShadowSlots(state);
            }
            state.hasAtlas = false;
            return false;
        }

        if (!state.initAttempted)
        {
            state.initAttempted = true;
            state.lastReport = std::chrono::steady_clock::now();

            if (!state.releaseEventsCreated)
            {
                for (os_event_t& event : state.releaseEvents)
                {
                    os_event_create(&event);
                }
                state.releaseEventsCreated = true;
            }

            if (state.renderer.initialize(d3d::get_device_name()))
            {
                NAU_LOG_INFO("mgpu omni shadows: the point light shadow maps are drawn by '{}', the scene by '{}'",
                             state.renderer.secondaryAdapterName(), state.renderer.primaryAdapterName());
            }
            else
            {
                NAU_LOG_WARNING("mgpu omni shadows: second GPU not available, Nau draws the point light shadow maps: {}",
                                state.renderer.lastError());
            }
        }

        if (!state.renderer.isReady())
        {
            return false;
        }
        if (m_mgpuShadows.autoSwitch.suspended)
        {
            return suspendMgpuRenderer(state);
        }

        ++state.framesSinceReport;
        releaseMgpuShadowSlots(state);

        // 1. The newest atlas the second GPU has finished goes into the atlas of Nau, with the constants it was drawn with.
        shadows::ReadyAtlas ready;
        if (state.renderer.takeReady(ready))
        {
            const auto pending = std::find_if(state.pendingFrames.begin(), state.pendingFrames.end(), [&](const auto& item)
            {
                return item.token == ready.userToken;
            });

            bool copied = false;
            if (pending != state.pendingFrames.end())
            {
                BaseTexture* target = m_omniShadows->getAtlas();
                copied = d3d::driver_command(DRV3D_COMMAND_D3D12_COPY_FROM_EXTERNAL_RESOURCE, ready.resource, target, nullptr) != 0;
                if (copied)
                {
                    m_omniShadows->markReadable();
                    d3d::driver_command(DRV3D_COMMAND_REGISTER_FRAME_COMPLETION_EVENT, &state.releaseEvents[ready.slot], nullptr, nullptr);
                    state.releasePending[ready.slot] = true;

                    state.hasAtlas = true;
                    state.omniAtlasSnapshot = pending->omniSnapshot;
                    m_omniBound = pending->omniSnapshot;
                    state.atlasFrame = pending->frame;
                    ++state.atlasesSinceReport;
                }
                else if (state.copyFailures++ == 0)
                {
                    NAU_LOG_ERROR("mgpu omni shadows: the driver did not copy the atlas of the second GPU, Nau draws the shadow maps");
                }
            }

            if (!copied)
            {
                state.renderer.release(ready.slot);
            }

            state.pendingFrames.erase(std::remove_if(state.pendingFrames.begin(), state.pendingFrames.end(), [&](const auto& item)
            {
                return item.token <= ready.userToken;
            }), state.pendingFrames.end());
        }

        // 2. The faces of this frame go to the second GPU if it has a free slot and the rate allows; it never waits.
        if (!mgpuSubmitDue(state))
        {
            return state.hasAtlas;
        }
        collectMgpuOmniShadowCasters(casters);

        const OmniShadows::AtlasInfo atlas = m_omniShadows->getAtlasInfo();
        shadows::FrameDesc frame;
        frame.atlasWidth = static_cast<uint32_t>(atlas.width);
        frame.atlasHeight = static_cast<uint32_t>(atlas.height);
        frame.format = shadows::AtlasFormat::Depth16;
        frame.cascades = state.cascades.data();
        frame.cascadeCount = static_cast<uint32_t>(state.cascades.size());
        frame.draws = state.draws.data();
        frame.drawCount = static_cast<uint32_t>(state.draws.size());
        frame.matrices = state.matrices.data();
        frame.matrixCount = static_cast<uint32_t>(state.matrices.size() / 16);
        frame.userToken = state.frame;
        if (state.renderer.submit(frame))
        {
            mgpuSubmitted(state);
            MgpuShadowsState::PendingFrame pending;
            pending.token = state.frame;
            pending.frame = state.frame;
            pending.omniSnapshot = m_omniShadows->makeSnapshot(casters);
            state.pendingFrames.push_back(pending);
        }

        // 3. Log.
        const shadows::RendererStats& stats = state.renderer.stats();
        if (stats.failed != state.reportedFailures)
        {
            NAU_LOG_ERROR("mgpu omni shadows: {} atlas(es) FAILED on the second GPU: {}", stats.failed - state.reportedFailures,
                          state.renderer.lastError());
            state.reportedFailures = stats.failed;
        }

        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - state.lastReport).count();
        if (elapsed >= MGPU_OMNI_REPORT_PERIOD_SECONDS)
        {
            NAU_LOG_INFO("mgpu omni shadows: {:.1f} atlases/s from '{}', {} draws ({} casters skipped), latency {:.1f} ms "
                         "(transfer {:.1f} ms), atlas age {} frames, {} completed, {} skipped (busy), {} failed",
                         static_cast<float>(state.atlasesSinceReport) / elapsed, state.renderer.secondaryAdapterName(), stats.lastDrawCount,
                         state.skippedCasters, stats.lastLatencyMs, stats.lastTransferMs,
                         state.hasAtlas ? state.frame - state.atlasFrame : 0, stats.completed, stats.skippedBusy,
                         stats.failed + state.copyFailures);
            state.atlasesSinceReport = 0;
            state.framesSinceReport = 0;
            state.lastReport = now;
        }

        return state.hasAtlas;
    }

#endif
}  // namespace nau::render
