// ambient_occlusion_nodes.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "render_window_impl.h"

namespace nau::render
{
    void RenderWindowImpl::createAmbientOcclusionNodes()
    {
        if (!m_ambientOcclusion)
        {
            return;
        }
        m_gBufferNodes.addNode(dabfg::register_node(nau::utils::format("ssao_{}", m_swapchain).c_str(), DABFG_PP_NODE_SRC,
                                                    [this](dabfg::Registry registry)
        {
            registry.orderMeAfter(nau::utils::format("z-prepass_{}", m_swapchain).c_str());
            registry.orderMeBefore(nau::utils::format("fill_gbuffer_{}", m_swapchain).c_str());
            registry.executionHas(dabfg::SideEffects::External);
            const auto createTarget = [this, &registry](const char* name, unsigned format = TEXFMT_R32F)
            {
                return registry.createTexture2d(nau::utils::format("{}_{}", name, m_swapchain).c_str(), dabfg::History::No,
                                                dabfg::Texture2dCreateInfo{format | TEXCF_RTARGET, registry.getResolution(m_resolutionName.c_str())})
                    .atStage(dabfg::Stage::POST_RASTER)
                    .useAs(dabfg::Usage::COLOR_ATTACHMENT)
                    .handle();
            };
            const dabfg::VirtualResourceHandle<BaseTexture, true, false> raw = createTarget("ssao_raw_texture");
            const dabfg::VirtualResourceHandle<BaseTexture, true, false> horizontal = createTarget("ssao_horizontal_texture");
            const dabfg::VirtualResourceHandle<BaseTexture, true, false> filtered = createTarget("ssao_texture", TEXFMT_R8);
            const dabfg::VirtualResourceHandle<BaseTexture, true, false> packedNormals = createTarget("ssao_normals_texture", TEXFMT_G16R16F);
            return [this, raw, horizontal, filtered, packedNormals]()
            {
                if (!m_graphicsScene->hasCamera())
                {
                    d3d::set_render_target(filtered.get(), 0);
                    d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
                    return;
                }
                const CameraNode& camera = m_graphicsScene->getMainCamera();
                d3d::setview(0, 0, m_gBuffer->getWidth(), m_gBuffer->getHeight(), 0.0f, 1.0f);
                m_ambientOcclusion->beginFrame(camera.getViewMatrix(), camera.getProjMatrixReverseZ(), m_windowHandle);
                d3d::set_render_target();
                d3d::set_depth(nullptr, DepthAccess::RW);
                d3d::resource_barrier({m_gBuffer->getDepth(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
                m_ambientOcclusion->renderFrame(raw.get(), horizontal.get(), filtered.get(),
                                                m_gBuffer->getDepth(), packedNormals.get());
            };
        }));
    }
}  // namespace nau::render
