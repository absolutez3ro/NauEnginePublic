// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <cstring>

#include "nau/diag/logging.h"
#include "render/daBfg/bfg.h"
#include "render_window_impl.h"

namespace nau::render
{
#if defined(NAU_ENABLE_MGPU)
    // MANUAL SWITCH of the Mgpu path of the render pipeline: a second GPU produces an image, it travels through a
    // shared heap to the Mgpu device on Nau's adapter and is written into a Nau texture (logged every
    // MGPU_TRANSFER_PERIOD_FRAMES frames). The value is applied once, when the first render graph is created;
    // nau::mgpu::bridge::setEnabled() can still change it at run time afterwards.
    // It only has an effect in a build with NAU_ENABLE_MGPU (preset win_vs2022_x64_mgpu) on a machine with two distinct
    // GPUs; with one GPU the bridge reports that it is unavailable and rendering is unchanged.
    constexpr bool MGPU_TRANSFER_ENABLED = true; // TEST BUILD (local change, not committed): was false
#endif

    void RenderWindowImpl::createMgpuNodes()
    {
#if defined(NAU_ENABLE_MGPU)
        static const bool switchApplied = (nau::mgpu::bridge::setEnabled(MGPU_TRANSFER_ENABLED), true);
        static_cast<void>(switchApplied);

        // TEST BUILD (local change, not committed): the node was added to m_debugNodes, but the debug stage is disabled
        // right after the render graph is created (GraphicsImpl::createRenderWindow), so it never ran. It is kept in
        // its own handle now and is always enabled.
        // The node exists even while the Mgpu path is switched off: it then returns right away (see updateMgpuTransfer).
        m_mgpuNode = (
            dabfg::register_node(nau::utils::format("{}_{}", "mgpu_transfer", m_swapchain).c_str(), DABFG_PP_NODE_SRC, [this](dabfg::Registry registry)
        {
            registry.orderMeAfter(nau::utils::format("{}_{}", "post_fx_nodes", m_swapchain).c_str());
            registry.executionHas(dabfg::SideEffects::External);

            return [this]()
            {
                updateMgpuTransfer();
            };
        }));
#endif
    }

#if defined(NAU_ENABLE_MGPU)

    void RenderWindowImpl::releaseMgpuTransfer()
    {
        m_mgpuTransfer.bridge.shutdown();
        m_mgpuTransfer.targetTexture.close();
        m_mgpuTransfer.stagingTexture.close();
        m_mgpuTransfer.initAttempted = false;
        m_mgpuTransfer.frameCounter = 0;
    }

    void RenderWindowImpl::updateMgpuTransfer()
    {
        auto& state = m_mgpuTransfer;

        if (!nau::mgpu::bridge::isEnabled())
        {
            if (state.initAttempted)
            {
                releaseMgpuTransfer();
                NAU_LOG_INFO("mgpu transfer: switched off");
            }
            return;
        }

        if (!state.initAttempted)
        {
            state.initAttempted = true;

            // The primary Mgpu device is the one on the adapter Nau renders on.
            if (!state.bridge.initialize(d3d::get_device_name()))
            {
                NAU_LOG_WARNING("mgpu transfer: not available: {}", state.bridge.lastError());
                return;
            }

            NAU_LOG_INFO("mgpu transfer: ready, primary adapter '{}', secondary adapter '{}'",
                         state.bridge.primaryAdapterName(), state.bridge.secondaryAdapterName());
        }

        if (!state.bridge.isReady())
        {
            return;
        }

        // A full transfer waits for both GPUs, so it runs once per MGPU_TRANSFER_PERIOD_FRAMES frames.
        const uint32_t frame = state.frameCounter++;
        if (frame % MGPU_TRANSFER_PERIOD_FRAMES != 0)
        {
            return;
        }

        const bool transferred = state.bridge.update(frame);
        const nau::mgpu::bridge::TransferInfo& info = state.bridge.info();

        if (!transferred)
        {
            NAU_LOG_ERROR("mgpu transfer: frame {} FAILED: {} (checksum {:016x}, expected {:016x})", frame,
                          state.bridge.lastError(), info.checksum, info.expectedChecksum);
            return;
        }

        // Received pixels -> Nau texture: through a system-memory texture, then a GPU copy into the target texture.
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

        NAU_LOG_INFO("mgpu transfer: frame {} OK, {}x{}, checksum {:016x}, written to 'mgpu_transfer_tex'", frame,
                     info.width, info.height, info.checksum);
    }

#endif
}  // namespace nau::render
