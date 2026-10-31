// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cmath>
#include <cstring>

#include "nau/app/global_properties.h"
#include "nau/diag/logging.h"
#include "nau/service/service_provider.h"

namespace nau::render
{
    namespace
    {
        template <typename TValue>
        TValue readSetting(const char* name, const TValue& fallback)
        {
            return getServiceProvider().get<GlobalProperties>().getValue<TValue>(name).value_or(fallback);
        }

        float readFiniteSetting(const char* name, float fallback, float minimum, float maximum)
        {
            const float value = readSetting(name, fallback);
            return eastl::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
        }
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
        MaterialAssetRef raw{"file:/res/materials/ssao.nmat_json"};
        MaterialAssetRef blur{"file:/res/materials/ssao_blur.nmat_json"};
        m_rawMaterial = co_await raw.getAssetViewTyped<MaterialAssetView>();
        if (!m_rawMaterial)
        {
            co_yield NauMakeError("Failed to load /res/materials/ssao.nmat_json");
        }
        m_blurMaterial = co_await blur.getAssetViewTyped<MaterialAssetView>();
        if (!m_blurMaterial)
        {
            co_yield NauMakeError("Failed to load /res/materials/ssao_blur.nmat_json");
        }
        m_rawMaterial->enableAutoSetTextures(false);
        m_blurMaterial->enableAutoSetTextures(false);

        const float radius = readFiniteSetting("render/ssao/radius", 0.5f, 0.0f, 10.0f);
        const float fadeStart = readFiniteSetting("render/ssao/fadeStart", 0.2f, 0.0f, 10.0f);
        const float fadeEnd = readFiniteSetting("render/ssao/fadeEnd", 1.0f, fadeStart + 0.001f, 20.0f);
        const float epsilon = readFiniteSetting("render/ssao/surfaceEpsilon", 0.05f, 0.0001f, fadeEnd);
        m_parameters = math::Vector4(radius, fadeStart, fadeEnd, epsilon);
        m_blurDepthThreshold = readFiniteSetting("render/ssao/blurDepthThreshold", 0.2f, 0.0f, 10.0f);
        m_blur = readSetting<bool>("render/ssao/blur", true);
        m_useSecondary = readSetting<bool>("render/ssao/useSecondary", false);
        if (m_useSecondary && d3d::is_multi_gpu_available())
        {
            ShaderAssetRef vertex{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"};
            ShaderAssetRef rawShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            m_vertexShader = co_await vertex.getAssetViewTyped<ShaderAssetView>();
            m_rawShader = co_await rawShader.getAssetViewTyped<ShaderAssetView>();
            if (!m_vertexShader || !m_rawShader)
            {
                m_useSecondary = false;
                NAU_LOG_ERROR("SSAO: secondary shaders unavailable, using GPU 0");
            }
        }
        if (!m_useSecondary || !d3d::is_multi_gpu_available())
        {
            NAU_LOG_INFO("SSAO: GPU 0, full resolution, RG16F prepass normals");
        }
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        resetSecondary();
    }

    void AmbientOcclusion::resetSecondary()
    {
        // Drain foreign queues before destroying any endpoint they may still reference.
        d3d::mgpu::destroySsaoProducer(m_secondary);
        m_secondary = nullptr;
        for (BaseTexture** texture : {&m_normalsSnapshot, &m_depthSnapshot, &m_receiving, &m_filtered})
        {
            if (*texture)
            {
                (*texture)->destroy();
                *texture = nullptr;
            }
        }
        m_secondaryReady = {};
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode, BaseTexture* normals, BaseTexture* depth)
    {
        if (m_mode != mode)
        {
            resetSecondary();
            m_secondaryAttempted = false;
            m_mode = mode;
        }
        m_normals = normals;
        m_depth = depth;
        m_secondaryReady = {};
        const math::Matrix4 inverseProjection = inverse(projection);
        m_rawMaterial->setProperty("default", "aoProjection", projection);
        m_rawMaterial->setProperty("default", "aoParameters", m_parameters);
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_blurMaterial.get()})
        {
            material->setProperty("default", "aoInverseProjection", inverseProjection);
            material->setProperty("default", "aoView", view);
        }
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        m_constants.filter[2] = m_blurDepthThreshold;
        if (m_mode == AmbientOcclusionMode::off || !m_useSecondary || !prepareSecondary(depth))
        {
            return;
        }
        const math::Vector4 depthParameters(projection.getElem(3, 2), projection.getElem(2, 3), projection.getElem(2, 2), m_blurDepthThreshold);
        for (const char* pipeline : {"halfBlur", "upsample"})
        {
            m_blurMaterial->setProperty(pipeline, "aoDepthParameters", depthParameters);
        }
        downsampleInputs();
        // Queue both input copies, GPU 1 AO, and its return copy now. GPU 0 keeps drawing.
        m_secondaryReady = d3d::mgpu::submitSsao(m_secondary, m_constants, m_normalsSnapshot, m_depthSnapshot, m_receiving);
    }

    void AmbientOcclusion::render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        if (m_mode == AmbientOcclusionMode::off)
        {
            d3d::set_render_target(output, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            d3d::set_render_target();
            return;
        }
        m_normals = normals;
        m_depth = depth;
        d3d::resource_barrier({normals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        if (m_secondaryReady.nativeFence && d3d::is_multi_gpu_available())
        {
            // This wait is recorded after G-buffer/CSM, immediately before consuming this frame's AO.
            d3d::mgpu::wait_for_primary_transfer(m_secondaryReady);
            m_blurMaterial->setProperty("halfBlur", "aoHalfControl", math::Vector4(m_blur ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f));
            drawSecondaryFilter(m_filtered, m_receiving, "halfBlur");
            drawSecondaryFilter(output, m_filtered, "upsample");
            return;
        }
        drawRaw(raw);
        drawBlur(horizontal, raw, true);
        drawBlur(output, horizontal, false);
    }

    bool AmbientOcclusion::prepareSecondary(BaseTexture* depth)
    {
        if (!d3d::is_multi_gpu_available() || !m_vertexShader || !m_rawShader)
        {
            return false;
        }
        TextureInfo info;
        depth->getinfo(info);
        const uint32_t width = (info.w + 1) / 2;
        const uint32_t height = (info.h + 1) / 2;
        if (m_primaryWidth != info.w || m_primaryHeight != info.h)
        {
            resetSecondary();
            m_secondaryWidth = width;
            m_secondaryHeight = height;
            m_primaryWidth = info.w;
            m_primaryHeight = info.h;
            m_secondaryAttempted = false;
        }
        if (!m_secondaryAttempted)
        {
            m_secondaryAttempted = true;
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* raw = m_rawShader->getShader();
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = width;
            desc.height = height;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (!m_secondary)
            {
                return false;
            }
            m_normalsSnapshot = d3d::create_tex(nullptr, width, height, TEXFMT_G16R16F | TEXCF_RTARGET, 1, u8"ssao_half_normals");
            m_depthSnapshot = d3d::create_tex(nullptr, width, height, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_half_depth");
            m_receiving = d3d::create_tex(nullptr, width, height, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_half_raw");
            m_filtered = d3d::create_tex(nullptr, width, height, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_half_filtered");
            if (!m_normalsSnapshot || !m_depthSnapshot || !m_receiving || !m_filtered)
            {
                d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_OUTOFMEMORY));
                resetSecondary();
                return false;
            }
            NAU_LOG_INFO("SSAO: same-frame GPU 1, half resolution {}x{}, RG16F normals, GPU 0 blur/upsample", width, height);
        }
        if (!m_secondary)
        {
            return false;
        }
        return true;
    }

    void AmbientOcclusion::downsampleInputs()
    {
        d3d::resource_barrier({m_normals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(m_normalsSnapshot, 0);
        d3d::set_render_target(1, m_depthSnapshot, 0);
        d3d::setview(0, 0, m_secondaryWidth, m_secondaryHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
        m_blurMaterial->bindPipeline("downsample");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(0, nullptr);
        d3d::settex(1, nullptr);
        d3d::set_render_target();
    }

    void AmbientOcclusion::drawSecondaryFilter(BaseTexture* target, BaseTexture* source, const char* pipeline)
    {
        d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_depthSnapshot, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(1, m_depthSnapshot);
        d3d::settex(2, source);
        if (target != m_filtered)
        {
            d3d::settex(3, m_depth);
        }
        m_blurMaterial->bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 1; slot <= 3; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::drawRaw(BaseTexture* target)
    {
        m_rawMaterial->setProperty("default", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, 1.0f));
        draw(target, *m_rawMaterial);
    }

    void AmbientOcclusion::drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal)
    {
        const float x = m_blur && horizontal ? 1.0f : 0.0f;
        const float y = m_blur && !horizontal ? 1.0f : 0.0f;
        m_blurMaterial->setProperty("default", "aoFilter", math::Vector4(x, y, m_blurDepthThreshold, 1.0f));
        d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        draw(target, *m_blurMaterial, source);
    }

    void AmbientOcclusion::draw(BaseTexture* target, MaterialAssetView& material, BaseTexture* source)
    {
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
        d3d::settex(2, source);
        material.bindPipeline("default");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 0; slot < 3; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::drawDebug(BaseTexture* target, BaseTexture* source)
    {
        d3d::set_render_target(target, 0);
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_srgb_backbuffer_write(false);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::settex(2, source);
        m_blurMaterial->bindPipeline("debug");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(2, nullptr);
    }

    bool AmbientOcclusion::isDebugView() const
    {
        return m_mode == AmbientOcclusionMode::debug;
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_mode != AmbientOcclusionMode::off ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    }
}  // namespace nau::render
