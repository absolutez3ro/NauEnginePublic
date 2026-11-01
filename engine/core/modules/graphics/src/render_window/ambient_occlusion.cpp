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

    AmbientOcclusion::Settings AmbientOcclusion::getInitialSettings()
    {
        Settings settings;
        settings.mode = static_cast<AmbientOcclusionMode>(eastl::min(readSetting<uint32_t>("render/ssao/mode", 1), 2u));
        settings.execution = readSetting<bool>("render/ssao/useSecondary", false)
                                 ? AmbientOcclusionExecution::multiGpu
                                 : AmbientOcclusionExecution::singleGpu;
        settings.blur = static_cast<AmbientOcclusionBlur>(eastl::min(readSetting<uint32_t>("render/ssao/blurMode", 0), 1u));
        return settings;
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
        // Load secondary shaders even when starting on GPU 0, so F8 can enable MGPU later.
        if (d3d::is_multi_gpu_available())
        {
            ShaderAssetRef vertex{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"};
            ShaderAssetRef rawShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            m_vertexShader = co_await vertex.getAssetViewTyped<ShaderAssetView>();
            m_rawShader = co_await rawShader.getAssetViewTyped<ShaderAssetView>();
            if (!m_vertexShader || !m_rawShader)
            {
                NAU_LOG_ERROR("SSAO: secondary shaders unavailable, using GPU 0");
            }
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
        for (BaseTexture** texture : {&m_normalsSnapshot, &m_depthSnapshot, &m_receiving, &m_filtered, &m_horizontal})
        {
            if (*texture)
            {
                (*texture)->destroy();
                *texture = nullptr;
            }
        }
        m_secondaryReady = {};
        m_usingSecondary = false;
        m_secondaryAttempted = false;
        m_primaryWidth = 0;
        m_primaryHeight = 0;
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, const Settings& settings, BaseTexture* normals, BaseTexture* depth)
    {
        if (m_settings.mode != settings.mode || m_settings.execution != settings.execution)
        {
            // Mode changes retire both GPUs' work before any shared endpoint is reused.
            resetSecondary();
        }
        m_logSettings |= m_settings.mode != settings.mode || m_settings.execution != settings.execution || m_settings.blur != settings.blur;
        m_settings = settings;
        m_normals = normals;
        m_depth = depth;
        m_secondaryReady = {};
        m_usingSecondary = false;
        const math::Matrix4 inverseProjection = inverse(projection);
        m_rawMaterial->setProperty("default", "aoProjection", projection);
        m_rawMaterial->setProperty("default", "aoParameters", m_parameters);
        m_rawMaterial->setProperty("default", "aoInverseProjection", inverseProjection);
        m_rawMaterial->setProperty("default", "aoView", view);
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        m_constants.filter[2] = m_blurDepthThreshold;
        if (m_settings.mode == AmbientOcclusionMode::off || !prepareTargets(depth))
        {
            logSettings();
            return;
        }
        const math::Vector4 depthParameters(projection.getElem(3, 2), projection.getElem(2, 3), projection.getElem(2, 2), m_blurDepthThreshold);
        for (const char* pipeline : {"halfBlur", "bogdanov", "upsample"})
        {
            m_blurMaterial->setProperty(pipeline, "aoDepthParameters", depthParameters);
        }
        downsampleInputs();
        if (m_settings.execution == AmbientOcclusionExecution::multiGpu && prepareSecondary())
        {
            // Queue both input copies, GPU 1 AO, and its return copy now. GPU 0 keeps drawing.
            m_secondaryReady = d3d::mgpu::submitSsao(m_secondary, m_constants, m_normalsSnapshot, m_depthSnapshot, m_receiving);
            m_usingSecondary = m_secondaryReady.nativeFence != nullptr;
            if (!m_usingSecondary)
            {
                // Drain partial transfers before the primary fallback writes the same raw target.
                d3d::mgpu::destroySsaoProducer(m_secondary);
                m_secondary = nullptr;
                m_logSettings = true;
            }
        }
        logSettings();
    }

    void AmbientOcclusion::render(BaseTexture* output)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        if (m_settings.mode == AmbientOcclusionMode::off || !m_receiving)
        {
            d3d::set_render_target(output, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            d3d::set_render_target();
            return;
        }
        if (m_usingSecondary)
        {
            // This wait is recorded after G-buffer/CSM, immediately before consuming this frame's AO.
            d3d::mgpu::wait_for_primary_transfer(m_secondaryReady);
        }
        else
        {
            // Use exactly the same half-size inputs, shader and R8 output as GPU 1.
            m_rawMaterial->setProperty("default", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, 1.0f));
            draw(m_receiving, *m_rawMaterial, "default");
        }
        filter(output);
    }

    bool AmbientOcclusion::prepareTargets(BaseTexture* depth)
    {
        TextureInfo info;
        depth->getinfo(info);
        if (m_normalsSnapshot && m_primaryWidth == info.w && m_primaryHeight == info.h)
        {
            return true;
        }
        resetSecondary();
        m_primaryWidth = info.w;
        m_primaryHeight = info.h;
        m_secondaryWidth = (info.w + 1) / 2;
        m_secondaryHeight = (info.h + 1) / 2;
        m_normalsSnapshot = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_G16R16F | TEXCF_RTARGET, 1, u8"ssao_half_normals");
        m_depthSnapshot = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_half_depth");
        m_receiving = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_half_raw");
        m_filtered = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_half_filtered");
        m_horizontal = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R16F | TEXCF_RTARGET, 1, u8"ssao_bogdanov_horizontal");
        if (!m_normalsSnapshot || !m_depthSnapshot || !m_receiving || !m_filtered || !m_horizontal)
        {
            NAU_LOG_ERROR("SSAO: could not allocate half-resolution comparison targets");
            resetSecondary();
            return false;
        }
        m_logSettings = true;
        return true;
    }

    bool AmbientOcclusion::prepareSecondary()
    {
        if (!d3d::is_multi_gpu_available() || !m_vertexShader || !m_rawShader)
        {
            return false;
        }
        if (!m_secondaryAttempted)
        {
            m_secondaryAttempted = true;
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* raw = m_rawShader->getShader();
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = m_secondaryWidth;
            desc.height = m_secondaryHeight;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (!m_secondary)
            {
                return false;
            }
            NAU_LOG_INFO("SSAO: same-frame GPU 1, half resolution {}x{}, RG16F normals, GPU 0 blur/upsample", m_secondaryWidth, m_secondaryHeight);
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

    void AmbientOcclusion::filter(BaseTexture* output)
    {
        if (m_settings.blur == AmbientOcclusionBlur::bogdanov && m_blur)
        {
            m_blurMaterial->setProperty("bogdanov", "aoHalfControl", math::Vector4(1.0f, 1.0f, 0.0f, 0.0f));
            draw(m_horizontal, *m_blurMaterial, "bogdanov", m_receiving);
            m_blurMaterial->setProperty("bogdanov", "aoHalfControl", math::Vector4(1.0f, 0.0f, 1.0f, 0.0f));
            draw(m_filtered, *m_blurMaterial, "bogdanov", m_horizontal);
        }
        else
        {
            m_blurMaterial->setProperty("halfBlur", "aoHalfControl", math::Vector4(m_blur ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f));
            draw(m_filtered, *m_blurMaterial, "halfBlur", m_receiving);
        }
        d3d::resource_barrier({m_depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        draw(output, *m_blurMaterial, "upsample", m_filtered);
    }

    void AmbientOcclusion::draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source)
    {
        d3d::resource_barrier({m_normalsSnapshot, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_depthSnapshot, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        if (source)
        {
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normalsSnapshot);
        d3d::settex(1, m_depthSnapshot);
        d3d::settex(2, source);
        d3d::settex(3, m_depth);
        material.bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 0; slot < 4; ++slot)
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
        return m_settings.mode == AmbientOcclusionMode::debug;
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_settings.mode != AmbientOcclusionMode::off ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    }

    const char* AmbientOcclusion::getExecutionLabel() const
    {
        if (m_settings.mode == AmbientOcclusionMode::off)
        {
            return "AO off";
        }
        if (m_usingSecondary)
        {
            return "MGPU, half AO";
        }
        return m_settings.execution == AmbientOcclusionExecution::multiGpu
                   ? "single GPU, half AO (MGPU unavailable)"
                   : "single GPU, half AO";
    }

    const char* AmbientOcclusion::getBlurLabel() const
    {
        if (!m_blur)
        {
            return "blur disabled";
        }
        return m_settings.blur == AmbientOcclusionBlur::bogdanov ? "Bogdanov 11 H+V" : "box 4x4 (depth)";
    }

    void AmbientOcclusion::logSettings()
    {
        if (!m_logSettings)
        {
            return;
        }
        NAU_LOG_INFO("SSAO comparison: {}, {}x{}, {}, display={}", getExecutionLabel(), m_secondaryWidth, m_secondaryHeight,
                     getBlurLabel(), static_cast<uint32_t>(m_settings.mode));
        m_logSettings = false;
    }
}  // namespace nau::render
