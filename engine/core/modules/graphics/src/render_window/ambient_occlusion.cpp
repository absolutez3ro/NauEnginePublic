// ambient_occlusion.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include "nau/app/global_properties.h"
#include "nau/3d/dag_drvDecl.h"
#include "nau/diag/logging.h"
#include "nau/service/service_provider.h"
#include "nau/shaders/shader_globals.h"

namespace nau::render
{
    namespace
    {
        template <typename TValue>
        TValue readSetting(const char* name, const TValue& fallback)
        {
            return getServiceProvider().get<GlobalProperties>().getValue<TValue>(name).value_or(fallback);
        }

        constexpr uint32_t AO_DIVISOR = 2;
        constexpr float DEPTH_THRESHOLD = 0.2f;
    }  // namespace

    bool AmbientOcclusion::isRequested()
    {
        return readSetting<bool>("render/ssao/enabled", false);
    }

    AmbientOcclusionMode AmbientOcclusion::getInitialMode()
    {
        return static_cast<AmbientOcclusionMode>(eastl::min(readSetting<uint32_t>("render/ssao/mode", 1), 2u));
    }

    async::Task<> AmbientOcclusion::loadAssets()
    {
        const eastl::string materialPath = readSetting<eastl::string>("render/ssao/material", {});
        const eastl::string postMaterialPath = readSetting<eastl::string>("render/ssao/postMaterial", {});
        if (materialPath.empty() || postMaterialPath.empty())
        {
            co_yield NauMakeError("SSAO requires project material and postMaterial asset paths");
        }
        MaterialAssetRef raw{materialPath};
        MaterialAssetRef post{postMaterialPath};
        m_rawMaterial = co_await raw.getAssetViewTyped<MaterialAssetView>();
        m_postMaterial = co_await post.getAssetViewTyped<MaterialAssetView>();
        if (!m_rawMaterial || !m_postMaterial)
        {
            co_yield NauMakeError("Failed to load the configured SSAO materials");
        }
        m_rawMaterial->enableAutoSetTextures(false);
        m_postMaterial->enableAutoSetTextures(false);

        d3d::SamplerInfo sampler;
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Linear;
        sampler.mip_map_mode = d3d::MipMapMode::Point;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = d3d::AddressMode::Clamp;
        m_linearClamp = d3d::create_sampler(sampler);
        m_postMaterial->setSampler("debugBilinear", "aoLinearClamp", m_linearClamp, 0);
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        releaseTargets();
    }

    void AmbientOcclusion::releaseTargets()
    {
        for (BaseTexture** target : {&m_halfDepth, &m_result})
        {
            if (*target)
            {
                (*target)->destroy();
                *target = nullptr;
            }
        }
    }

    bool AmbientOcclusion::prepareTargets(BaseTexture& depth)
    {
        TextureInfo info;
        depth.getinfo(info);
        const uint32_t width = (info.w + AO_DIVISOR - 1) / AO_DIVISOR;
        const uint32_t height = (info.h + AO_DIVISOR - 1) / AO_DIVISOR;
        if (m_result && width == m_width && height == m_height)
        {
            return true;
        }
        releaseTargets();
        if (!width || !height)
        {
            return false;
        }
        m_halfDepth = d3d::create_tex(nullptr, width, height, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ao_depth");
        m_result = d3d::create_tex(nullptr, width, height, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ao_result");
        if (!m_halfDepth || !m_result)
        {
            releaseTargets();
            return false;
        }
        m_width = width;
        m_height = height;
        return true;
    }

    void AmbientOcclusion::render(const math::Matrix4& projection, BaseTexture& depth, AmbientOcclusionMode mode)
    {
        m_mode = mode;
        m_valid = false;
        if (mode == AmbientOcclusionMode::off || !prepareTargets(depth))
        {
            return;
        }

        m_projection = projection;
        m_inverseProjection = inverse(projection);
        m_fullDepth = &depth;
        SCOPE_RENDER_TARGET;
        SCOPE_VIEWPORT;
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        draw(*m_halfDepth, *m_postMaterial, "downsample", depth);
        draw(*m_result, *m_rawMaterial, "default", *m_halfDepth);
        m_valid = true;
    }

    void AmbientOcclusion::draw(BaseTexture& target, MaterialAssetView& material, const char* pipeline, BaseTexture& depth, BaseTexture* source)
    {
        d3d::resource_barrier({&depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        if (source)
        {
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::set_render_target(&target, 0);
        TextureInfo info;
        target.getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(1, &depth);
        d3d::settex(2, source);
        d3d::settex(3, m_fullDepth);

        const float width = static_cast<float>(m_width);
        const float height = static_cast<float>(m_height);
        material.setProperty(pipeline, "aoProjection", m_projection);
        material.setProperty(pipeline, "aoInverseProjection", m_inverseProjection);
        material.setProperty(pipeline, "aoSizes", math::Vector4(width, height, width, height));
        material.setProperty(pipeline, "aoFilter", math::Vector4(width, height, DEPTH_THRESHOLD, 1.0f));
        material.bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::set_const_buffer(STAGE_PS, 0, nullptr);
        for (int slot = 1; slot < 4; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::drawDebug(BaseTexture& target)
    {
        d3d::set_depth(nullptr, DepthAccess::RW);
        draw(target, *m_postMaterial, "debugBilinear", *m_halfDepth, m_result);
    }

    bool AmbientOcclusion::isDebugView() const
    {
        return m_valid && m_mode == AmbientOcclusionMode::debug;
    }

    void AmbientOcclusion::invalidate()
    {
        m_valid = false;
        m_fullDepth = nullptr;
    }

    void AmbientOcclusion::configureResolveSampler(MaterialAssetView& material) const
    {
        for (const eastl::string& pipeline : material.getPipelineNames())
        {
            material.setSampler(pipeline, "ssaoLinearClamp", m_linearClamp, 6);
        }
    }

    void AmbientOcclusion::bindResolveInputs(BaseTexture& fallbackAo, BaseTexture& fallbackDepth) const
    {
        if (m_valid)
        {
            d3d::resource_barrier({m_result, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            d3d::resource_barrier({m_halfDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            const math::Vector4 reconstruction(AO_DIVISOR, DEPTH_THRESHOLD, m_projection[3][2], m_projection[2][2]);
            shader_globals::setVariable("ssaoReconstruction", &reconstruction);
        }
        d3d::settex(6, m_valid ? m_result : &fallbackAo);
        d3d::settex(7, m_valid ? m_halfDepth : &fallbackDepth);
        const math::Vector4 control(m_valid ? 1.0f : 0.0f);
        shader_globals::setVariable("ssaoControl", &control);
    }
}  // namespace nau::render
