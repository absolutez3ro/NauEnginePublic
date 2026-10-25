// ambient_occlusion_nodes.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "render_window_impl.h"

namespace nau::render
{
    void RenderWindowImpl::createAmbientOcclusionNodes()
    {
        using TextureHandle = dabfg::VirtualResourceHandle<BaseTexture, true, false>;
        using ReadTextureHandle = dabfg::VirtualResourceHandle<const BaseTexture, true, false>;

        if (!m_ambientOcclusion)
        {
            return;
        }
        m_gBufferNodes.addNode(dabfg::register_node(nau::utils::format("ssao_submit_{}", m_swapchain).c_str(), DABFG_PP_NODE_SRC,
                                                    [this](dabfg::Registry registry)
        {
            registry.orderMeAfter(nau::utils::format("z-prepass_{}", m_swapchain).c_str());
            registry.orderMeBefore(nau::utils::format("fill_gbuffer_{}", m_swapchain).c_str());
            registry.executionHas(dabfg::SideEffects::External);
            const auto createTarget = [this, &registry](const char* name, uint32_t format = TEXFMT_R32F, bool fullSize = false)
            {
                return registry.createTexture2d(nau::utils::format("{}_{}", name, m_swapchain).c_str(), dabfg::History::No,
                                                dabfg::Texture2dCreateInfo{format | TEXCF_RTARGET, registry.getResolution(fullSize ? m_resolutionName.c_str() : nau::utils::format("{}_ssao", m_resolutionName).c_str())})
                    .atStage(dabfg::Stage::POST_RASTER)
                    .useAs(dabfg::Usage::COLOR_ATTACHMENT)
                    .handle();
            };
            const TextureHandle raw = createTarget("ssao_raw_texture");
            const TextureHandle horizontal = createTarget("ssao_horizontal_texture");
            const TextureHandle filtered = createTarget("ssao_low_texture", TEXFMT_R8);
            const TextureHandle packedNormals = createTarget("ssao_normals_texture", TEXFMT_G16R16F);
            const TextureHandle depth = createTarget("ssao_depth_texture");
            const TextureHandle fullDepth = createTarget("ssao_full_depth_texture", TEXFMT_R32F, true);
            return [this, raw, horizontal, filtered, packedNormals, depth, fullDepth]()
            {
                if (!m_graphicsScene->hasCamera())
                {
                    return;
                }
                const CameraNode& camera = m_graphicsScene->getMainCamera();
                d3d::setview(0, 0, m_gBuffer->getWidth(), m_gBuffer->getHeight(), 0.0f, 1.0f);
                m_ambientOcclusion->beginFrame(camera.getViewMatrix(), camera.getProjMatrixReverseZ(), m_windowHandle);
                d3d::set_render_target();
                d3d::set_depth(nullptr, DepthAccess::RW);
                d3d::resource_barrier({m_gBuffer->getDepth(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
                m_ambientOcclusion->submitFrame(raw.get(), horizontal.get(), filtered.get(),
                                                m_gBuffer->getDepth(), packedNormals.get(), depth.get(), fullDepth.get());
            };
        }));

        m_gBufferNodes.addNode(dabfg::register_node(nau::utils::format("ssao_receive_{}", m_swapchain).c_str(), DABFG_PP_NODE_SRC,
                                                    [this](dabfg::Registry registry)
        {
            registry.orderMeAfter(nau::utils::format("CSM_{}", m_swapchain).c_str());
            registry.orderMeBefore(nau::utils::format("gbuffer_resolve_{}", m_swapchain).c_str());
            registry.executionHas(dabfg::SideEffects::External);
            const auto modifyTarget = [this, &registry](const char* name)
            {
                return registry.modifyTexture(nau::utils::format("{}_{}", name, m_swapchain).c_str())
                    .atStage(dabfg::Stage::POST_RASTER)
                    .useAs(dabfg::Usage::COLOR_ATTACHMENT)
                    .handle();
            };
            const TextureHandle raw = modifyTarget("ssao_raw_texture");
            const TextureHandle horizontal = modifyTarget("ssao_horizontal_texture");
            const TextureHandle filtered = modifyTarget("ssao_low_texture");
            const TextureHandle normals = modifyTarget("ssao_normals_texture");
            const ReadTextureHandle depth = registry.readTexture(nau::utils::format("ssao_depth_texture_{}", m_swapchain).c_str())
                                                .atStage(dabfg::Stage::PS_OR_CS)
                                                .useAs(dabfg::Usage::SHADER_RESOURCE)
                                                .handle();
            const ReadTextureHandle fullDepth = registry.readTexture(nau::utils::format("ssao_full_depth_texture_{}", m_swapchain).c_str())
                                                    .atStage(dabfg::Stage::PS_OR_CS)
                                                    .useAs(dabfg::Usage::SHADER_RESOURCE)
                                                    .handle();
            const TextureHandle output = registry.createTexture2d(nau::utils::format("ssao_texture_{}", m_swapchain).c_str(), dabfg::History::No,
                                                                  dabfg::Texture2dCreateInfo{TEXFMT_R8 | TEXCF_RTARGET, registry.getResolution(m_resolutionName.c_str())})
                                             .atStage(dabfg::Stage::POST_RASTER)
                                             .useAs(dabfg::Usage::COLOR_ATTACHMENT)
                                             .handle();
            return [this, raw, horizontal, filtered, normals, depth, fullDepth, output]()
            {
                if (!m_graphicsScene->hasCamera())
                {
                    d3d::set_render_target(output.get(), 0);
                    d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
                    return;
                }
                // Keep normal and depth allocations live through late fallback and refill.
                static_cast<void>(normals.get());
                static_cast<void>(depth.get());
                static_cast<void>(fullDepth.get());
                d3d::set_render_target();
                d3d::set_depth(nullptr, DepthAccess::RW);
                m_ambientOcclusion->resolveFrame(raw.get(), horizontal.get(), filtered.get(), output.get());
            };
        }));
    }
}  // namespace nau::render
