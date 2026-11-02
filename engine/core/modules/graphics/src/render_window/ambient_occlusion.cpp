// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cmath>
#include <cstring>

#include "nau/app/global_properties.h"
#include "nau/3d/ssao_random_texture.h"
#include "nau/3d/dag_drv3dCmd.h"
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
        if (readSetting<uint32_t>("render/ssao/variant", 0) == 1)
        {
            settings.pipeline.inputs = AmbientOcclusionInputs::full;
            settings.pipeline.algorithm = AmbientOcclusionVariant::bogdanov;
            settings.pipeline.blur = AmbientOcclusionBlur::gaussian;
            settings.pipeline.upsample = AmbientOcclusionUpsample::bilinear;
        }
        auto& p = settings.pipeline;
        p.inputs = static_cast<AmbientOcclusionInputs>(eastl::min(readSetting<uint32_t>("render/ssao/inputs", static_cast<uint32_t>(p.inputs)), 1u));
        p.algorithm = static_cast<AmbientOcclusionVariant>(eastl::min(readSetting<uint32_t>("render/ssao/algorithm", static_cast<uint32_t>(p.algorithm)), 1u));
        p.normals = static_cast<AmbientOcclusionNormals>(eastl::min(readSetting<uint32_t>("render/ssao/normals", 0), 1u));
        p.blur = static_cast<AmbientOcclusionBlur>(eastl::min(readSetting<uint32_t>("render/ssao/blurMethod", readSetting<bool>("render/ssao/blur", true) ? static_cast<uint32_t>(p.blur) : 0u), 4u));
        p.blurExecution = readSetting<bool>("render/ssao/blurOnSecondary", false) ? AmbientOcclusionExecution::multiGpu : AmbientOcclusionExecution::singleGpu;
        p.upsample = static_cast<AmbientOcclusionUpsample>(eastl::min(readSetting<uint32_t>("render/ssao/upsample", static_cast<uint32_t>(p.upsample)), 2u));
        p.packedNormals = readSetting<bool>("render/ssao/packedNormals", false);
        p.linearDepth16 = readSetting<bool>("render/ssao/linearDepth16", false);
        p.eightSamples = readSetting<bool>("render/ssao/eightSamples", false);
        p.weightedUpsample = readSetting<bool>("render/ssao/weightedUpsample", false);
        p.rejectOutside = readSetting<bool>("render/ssao/rejectOutside", false);
        p.quarterResolution = readSetting<bool>("render/ssao/quarterResolution", false);
        p.gatherUpsample = readSetting<bool>("render/ssao/gatherUpsample", false);
        p.waitForSecondary = readSetting<bool>("render/ssao/waitForSecondary", true);
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
        d3d::SamplerInfo sampler;
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Linear;
        sampler.mip_map_mode = d3d::MipMapMode::Point;
        m_linearWrap = d3d::create_sampler(sampler);
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = d3d::AddressMode::Clamp;
        m_linearClamp = d3d::create_sampler(sampler);
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_blurMaterial.get()})
        {
            for (const auto& pipeline : material->getPipelineNames())
            {
                material->setSampler(pipeline, "aoLinearClamp", m_linearClamp, 0);
                material->setSampler(pipeline, "aoLinearWrap", m_linearWrap, 1);
            }
        }

        const float radius = readFiniteSetting("render/ssao/radius", 0.5f, 0.0f, 10.0f);
        const float fadeStart = readFiniteSetting("render/ssao/fadeStart", 0.2f, 0.0f, 10.0f);
        const float fadeEnd = readFiniteSetting("render/ssao/fadeEnd", 1.0f, fadeStart + 0.001f, 20.0f);
        const float epsilon = readFiniteSetting("render/ssao/surfaceEpsilon", 0.05f, 0.0001f, fadeEnd);
        m_parameters = math::Vector4(radius, fadeStart, fadeEnd, epsilon);
        m_blurDepthThreshold = readFiniteSetting("render/ssao/blurDepthThreshold", 0.2f, 0.0f, 10.0f);
        // Load secondary shaders even when starting on GPU 0, so F8 can enable MGPU later.
        if (d3d::is_multi_gpu_available())
        {
            ShaderAssetRef vertex{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"};
            ShaderAssetRef rawShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            ShaderAssetRef filterShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao_blur.ps.pshalfblur]"};
            m_vertexShader = co_await vertex.getAssetViewTyped<ShaderAssetView>();
            m_rawShader = co_await rawShader.getAssetViewTyped<ShaderAssetView>();
            m_filterShader = co_await filterShader.getAssetViewTyped<ShaderAssetView>();
            if (!m_vertexShader || !m_rawShader || !m_filterShader)
            {
                NAU_LOG_ERROR("SSAO: secondary shaders unavailable, using GPU 0");
            }
        }
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        resetSecondary();
        for (auto& timing : m_waitTimings)
        {
            d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &timing.begin, nullptr, nullptr);
            d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &timing.end, nullptr, nullptr);
        }
        if (m_randomTexture)
        {
            m_randomTexture->destroy();
        }
    }

    void AmbientOcclusion::resetSecondary()
    {
        // Drain foreign queues before destroying any endpoint they may still reference.
        d3d::mgpu::destroySsaoProducer(m_secondary);
        m_secondary = nullptr;
        m_resolveSource = nullptr;
        m_asyncValid = false;
        m_drawRaw = m_drawDepth = m_drawNormals = nullptr;
        for (BaseTexture** texture : {&m_normalsSnapshot, &m_depthSnapshot, &m_receiving, &m_filtered, &m_horizontal, &m_fullOutput, &m_asyncRaw, &m_asyncDepth, &m_asyncNormals})
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
        m_inputWidth = 0;
        m_inputHeight = 0;
        m_secondaryWidth = 0;
        m_secondaryHeight = 0;
    }

    void AmbientOcclusion::beginFrame(uint32_t width, uint32_t height, const Settings& settings)
    {
        const auto& old = m_settings.pipeline;
        const auto& next = settings.pipeline;
        pollWaitTimings();
        const bool asyncChanged = old.waitForSecondary != next.waitForSecondary ||
            ((!old.waitForSecondary || !next.waitForSecondary) &&
             (old != next || m_settings.execution != settings.execution));
        if (old.waitForSecondary != next.waitForSecondary || m_settings.mode != settings.mode || m_settings.execution != settings.execution)
        {
            ++m_timingGeneration;
            m_waitMilliseconds = -1.f;
        }
        const bool layoutChanged = old.inputs != next.inputs || old.normals != next.normals ||
            old.packedNormals != next.packedNormals || old.linearDepth16 != next.linearDepth16 ||
            old.quarterResolution != next.quarterResolution;
        m_logSettings |= old != next || m_settings.execution != settings.execution || m_settings.mode != settings.mode;
        if (layoutChanged || asyncChanged) resetSecondary();
        m_settings = settings;
        prepareTargets(width, height);
        if (!next.waitForSecondary && d3d::is_multi_gpu_available() && !m_asyncRaw && m_receiving)
        {
            m_asyncRaw = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_async_ready");
            m_asyncDepth = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (next.linearDepth16 ? TEXFMT_R16F : TEXFMT_R32F) | TEXCF_RTARGET, 1, u8"ssao_async_depth");
            m_asyncNormals = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (next.packedNormals ? TEXFMT_R8G8 : TEXFMT_G16R16F) | TEXCF_RTARGET, 1, u8"ssao_async_normals");
        }
        // A full-size AO allocation exists only for a separate reconstruction pass.
        if (next.upsample != AmbientOcclusionUpsample::separate && m_fullOutput)
        {
            m_fullOutput->destroy();
            m_fullOutput = nullptr;
        }
        if (next.upsample == AmbientOcclusionUpsample::separate && !m_fullOutput && m_receiving)
            m_fullOutput = d3d::create_tex(nullptr, width, height, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_full_output");
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth)
    {
        m_normals = normals;
        m_depth = depth;
        if (m_settings.pipeline.waitForSecondary) m_secondaryReady = {};
        m_usingSecondary = false;
        m_drawRaw = m_receiving;
        m_drawDepth = m_depthSnapshot;
        m_drawNormals = m_normalsSnapshot;
        const math::Matrix4 inverseProjection = inverse(projection);
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        m_constants.filter[2] = m_blurDepthThreshold;
        if (m_settings.mode == AmbientOcclusionMode::off || !m_receiving)
        {
            logSettings();
            return;
        }
        const auto& p = m_settings.pipeline;
        m_constants.options[0] = p.normals == AmbientOcclusionNormals::reconstructed ? 1.f : 0.f;
        m_constants.options[1] = p.packedNormals ? 1.f : 0.f;
        m_constants.options[2] = p.linearDepth16 ? 1.f : 0.f;
        m_constants.options[3] = p.eightSamples ? 8.f : 14.f;
        m_constants.pipeline[0] = static_cast<float>(p.blur);
        m_constants.pipeline[1] = 0.f;
        m_constants.pipeline[2] = p.rejectOutside ? 1.f : 0.f;
        m_constants.pipeline[3] = static_cast<float>(p.algorithm);
        m_constants.filter[3] = p.blurExecution == AmbientOcclusionExecution::multiGpu ? 2.f : 1.f;
        m_constants.sizes[0] = static_cast<float>(m_secondaryWidth);
        m_constants.sizes[1] = static_cast<float>(m_secondaryHeight);
        m_constants.sizes[2] = static_cast<float>(m_inputWidth);
        m_constants.sizes[3] = static_cast<float>(m_inputHeight);
        m_constants.reconstruction[0] = p.blur == AmbientOcclusionBlur::bilateralUpsample ? 2.f :
            (p.weightedUpsample ? 3.f : (p.upsample == AmbientOcclusionUpsample::bilinear ? 1.f : 0.f));
        m_constants.reconstruction[1] = p.gatherUpsample ? 1.f : 0.f;
        m_constants.reconstruction[2] = p.quarterResolution ? 4.f : 2.f;
        m_constants.reconstruction[3] = m_blurDepthThreshold;
        m_constants.filter[0] = static_cast<float>(m_secondaryWidth);
        m_constants.filter[1] = static_cast<float>(m_secondaryHeight);
        const bool async = !p.waitForSecondary && m_settings.execution == AmbientOcclusionExecution::multiGpu &&
            m_asyncRaw && m_asyncDepth && m_asyncNormals && prepareSecondary();
        if (async && m_secondaryReady.nativeFence)
        {
            if (!d3d::mgpu::is_primary_transfer_complete(m_secondaryReady))
            {
                // Keep all input/output endpoints untouched while GPU 1 is still using them.
                m_usingSecondary = true;
                logSettings();
                return;
            }
            m_asyncValid = m_asyncRaw->update(m_receiving) && m_asyncDepth->update(m_depthSnapshot) && m_asyncNormals->update(m_normalsSnapshot);
            m_asyncConstants = m_submittedConstants;
            m_secondaryReady = {};
        }
        downsampleInputs();
        if (m_settings.execution == AmbientOcclusionExecution::multiGpu && (p.waitForSecondary || async) && prepareSecondary())
        {
            // Queue both input copies, GPU 1 AO, and its return copy now. GPU 0 keeps drawing.
            m_secondaryReady = d3d::mgpu::submitSsao(m_secondary, m_constants, m_normalsSnapshot, m_depthSnapshot, m_receiving);
            m_usingSecondary = m_secondaryReady.nativeFence != nullptr;
            m_submittedConstants = m_constants;
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
            if (m_settings.pipeline.waitForSecondary)
                waitForSecondary();
            else if (m_asyncValid)
            {
                m_drawRaw = m_asyncRaw;
                m_drawDepth = m_asyncDepth;
                m_drawNormals = m_asyncNormals;
                m_constants = m_asyncConstants;
            }
            else
            {
                // No completed result yet: render this frame without AO, without a queue wait.
                m_resolveSource = nullptr;
                d3d::set_render_target(output, 0);
                d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
                d3d::set_render_target();
                return;
            }
        }
        else
        {
            // Match GPU 1's selected input size, shader and half-size R8 output.
            draw(m_receiving, *m_rawMaterial, "default");
        }
        filter(output);
    }

    bool AmbientOcclusion::prepareTargets(uint32_t width, uint32_t height)
    {
        TextureInfo info{};
        info.w = width;
        info.h = height;
        if (m_normalsSnapshot && m_primaryWidth == info.w && m_primaryHeight == info.h)
        {
            return true;
        }
        resetSecondary();
        m_primaryWidth = info.w;
        m_primaryHeight = info.h;
        const uint32_t divisor = m_settings.pipeline.quarterResolution ? 4 : 2;
        m_secondaryWidth = (info.w + divisor - 1) / divisor;
        m_secondaryHeight = (info.h + divisor - 1) / divisor;
        m_inputWidth = m_settings.pipeline.inputs == AmbientOcclusionInputs::full ? info.w : m_secondaryWidth;
        m_inputHeight = m_settings.pipeline.inputs == AmbientOcclusionInputs::full ? info.h : m_secondaryHeight;
        if (!createRandomTexture())
        {
            return false;
        }
        m_normalsSnapshot = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (m_settings.pipeline.packedNormals ? TEXFMT_R8G8 : TEXFMT_G16R16F) | TEXCF_RTARGET, 1, u8"ssao_input_normals");
        m_depthSnapshot = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (m_settings.pipeline.linearDepth16 ? TEXFMT_R16F : TEXFMT_R32F) | TEXCF_RTARGET, 1, u8"ssao_input_depth");
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
        if (!d3d::is_multi_gpu_available() || !m_vertexShader || !m_rawShader || !m_filterShader)
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
            desc.inputWidth = m_inputWidth;
            desc.inputHeight = m_inputHeight;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            desc.reconstructNormals = m_settings.pipeline.normals == AmbientOcclusionNormals::reconstructed;
            desc.packedNormals = m_settings.pipeline.packedNormals;
            desc.linearDepth16 = m_settings.pipeline.linearDepth16;
            const Shader* filter = m_filterShader->getShader();
            desc.filterBytecode = filter->bytecode.data();
            desc.filterBytecodeSize = filter->bytecode.size();
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
        d3d::setview(0, 0, m_inputWidth, m_inputHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
        setConstants(*m_blurMaterial, "downsample");
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
        const auto& p = m_settings.pipeline;
        const bool blurOnSecondary = m_usingSecondary && p.blurExecution == AmbientOcclusionExecution::multiGpu;
        m_resolveSource = m_drawRaw;
        if (!blurOnSecondary && p.blur != AmbientOcclusionBlur::off && p.blur != AmbientOcclusionBlur::bilateralUpsample)
        {
            m_constants.pipeline[1] = 0.f;
            draw(p.blur == AmbientOcclusionBlur::gaussian ? m_horizontal : m_filtered, *m_blurMaterial, "halfBlur", m_drawRaw);
            if (p.blur == AmbientOcclusionBlur::gaussian)
            {
                m_constants.pipeline[1] = 1.f;
                draw(m_filtered, *m_blurMaterial, "halfBlur", m_horizontal);
            }
            m_resolveSource = m_filtered;
        }
        if (m_fullOutput)
        {
            d3d::resource_barrier({m_depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            draw(m_fullOutput, *m_blurMaterial, "upsample", m_resolveSource);
        }
        // Only a small graph token is needed when resolve consumes the low-resolution AO.
        d3d::set_render_target(output, 0);
        d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
        d3d::set_render_target();
    }

    void AmbientOcclusion::setConstants(MaterialAssetView& material, const char* pipeline)
    {
        const auto matrix = [](const float* values) { math::Matrix4 m; std::memcpy(&m, values, sizeof(m)); return m; };
        const auto vector = [](const float* v) { return math::Vector4(v[0], v[1], v[2], v[3]); };
        material.setProperty(pipeline, "aoProjection", matrix(m_constants.projection));
        material.setProperty(pipeline, "aoInverseProjection", matrix(m_constants.inverseProjection));
        material.setProperty(pipeline, "aoView", matrix(m_constants.view));
        material.setProperty(pipeline, "aoParameters", vector(m_constants.parameters));
        material.setProperty(pipeline, "aoFilter", vector(m_constants.filter));
        material.setProperty(pipeline, "aoOptions", vector(m_constants.options));
        material.setProperty(pipeline, "aoPipeline", vector(m_constants.pipeline));
        material.setProperty(pipeline, "aoSizes", vector(m_constants.sizes));
        material.setProperty(pipeline, "aoReconstruction", vector(m_constants.reconstruction));
    }

    void AmbientOcclusion::draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source)
    {
        d3d::resource_barrier({m_drawNormals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_drawDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        if (source)
        {
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_drawNormals);
        d3d::settex(1, m_drawDepth);
        d3d::settex(2, source);
        d3d::settex(3, m_depth);
        d3d::settex(4, m_randomTexture);
        setConstants(material, pipeline);
        material.bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 0; slot < 5; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::drawDebug(BaseTexture* target, BaseTexture*)
    {
        d3d::set_depth(nullptr, DepthAccess::RW);
        if (!m_resolveSource)
        {
            d3d::set_render_target(target, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            return;
        }
        // The same reconstruction is used in debug and lighting for every combination.
        draw(target, *m_blurMaterial, "debugBilinear", m_resolveSource);
    }

    bool AmbientOcclusion::isDebugView() const
    {
        return m_settings.mode == AmbientOcclusionMode::debug;
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_settings.mode != AmbientOcclusionMode::off && m_resolveSource ? 1.f : 0.f, m_fullOutput ? -1.f : m_constants.reconstruction[0], m_settings.pipeline.linearDepth16 ? 1.f : 0.f, m_settings.pipeline.gatherUpsample ? 1.f : 0.f);
    }

    void AmbientOcclusion::pollWaitTimings()
    {
        uint64_t frequency = 0;
        d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &frequency, nullptr, nullptr);
        for (auto& timing : m_waitTimings)
        {
            uint64_t begin = 0, end = 0;
            if (timing.pending && d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.begin, &begin, nullptr) &&
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.end, &end, nullptr))
            {
                if (timing.generation == m_timingGeneration && timing.sequence > m_lastWaitSequence && frequency && end >= begin)
                {
                    m_waitMilliseconds = static_cast<float>(1000.0 * static_cast<double>(end - begin) / frequency);
                    m_lastWaitSequence = timing.sequence;
                }
                timing.pending = false;
            }
        }
    }

    void AmbientOcclusion::waitForSecondary()
    {
        WaitTiming* sample = nullptr;
        for (auto& timing : m_waitTimings)
            if (!timing.pending) { sample = &timing; break; }
        // Timestamp queries run on GPU 0 on either side of the graphics-queue Wait.
        // CPU-side duration of queue submission would not measure GPU idle time.
        if (sample) d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &sample->begin, nullptr, nullptr);
        d3d::mgpu::wait_for_primary_transfer(m_secondaryReady);
        if (sample)
        {
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &sample->end, nullptr, nullptr);
            sample->generation = m_timingGeneration;
            sample->sequence = ++m_nextWaitSequence;
            sample->pending = true;
        }
    }

    const char* AmbientOcclusion::getWaitLabel() const
    {
        if (m_settings.mode == AmbientOcclusionMode::off) return "n/a (AO off)";
        if (!m_usingSecondary) return "n/a (no GPU1 AO)";
        if (!m_settings.pipeline.waitForSecondary) return "OFF (async)";
        return m_waitMilliseconds < 0.f ? "pending" : nullptr;
    }

    const char* AmbientOcclusion::getExecutionLabel() const
    {
        if (m_settings.mode == AmbientOcclusionMode::off)
        {
            return "AO off";
        }
        if (m_usingSecondary)
        {
            return m_settings.pipeline.quarterResolution ? "MGPU, quarter AO" : "MGPU, half AO";
        }
        return m_settings.execution == AmbientOcclusionExecution::multiGpu
                   ? (m_settings.pipeline.quarterResolution ? "single GPU, quarter AO (MGPU unavailable)" : "single GPU, half AO (MGPU unavailable)")
                   : (m_settings.pipeline.quarterResolution ? "single GPU, quarter AO" : "single GPU, half AO");
    }

    const char* AmbientOcclusion::getVariantLabel() const
    {
        return m_settings.pipeline.algorithm == AmbientOcclusionVariant::bogdanov ? "Bogdanov AO / independent stages" : "Nau AO / independent stages";
    }

    void AmbientOcclusion::logSettings()
    {
        if (!m_logSettings) return;
        const auto& p = m_settings.pipeline;
        NAU_LOG_INFO("SSAO pipeline: {}, AO={}x{}, inputs={}x{}, algorithm={}, normals={}, blur={}, blurGPU={}, upsample={}, RG8={}, R16F={}, samples={}, weighted={}, reject={}, gatherUp={}, wait={}, mode={}",
            getExecutionLabel(), m_secondaryWidth, m_secondaryHeight, m_inputWidth, m_inputHeight,
            static_cast<uint32_t>(p.algorithm), static_cast<uint32_t>(p.normals), static_cast<uint32_t>(p.blur),
            static_cast<uint32_t>(p.blurExecution), static_cast<uint32_t>(p.upsample), p.packedNormals, p.linearDepth16,
            p.eightSamples ? 8 : 14, p.weightedUpsample, p.rejectOutside, p.gatherUpsample, p.waitForSecondary, static_cast<uint32_t>(m_settings.mode));
        m_logSettings = false;
    }

    BaseTexture* AmbientOcclusion::getResolveTexture(BaseTexture* fallback) const
    {
        return m_fullOutput ? m_fullOutput : (m_resolveSource ? m_resolveSource : fallback);
    }

    void AmbientOcclusion::bindResolveInputs()
    {
        if (BaseTexture* source = getResolveTexture(nullptr))
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_drawDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::settex(7, m_drawDepth);
        const math::Vector4 reconstruction(m_constants.reconstruction[2], m_blurDepthThreshold,
            m_constants.projection[14], m_constants.projection[10]);
        shader_globals::setVariable("ssaoReconstruction", &reconstruction);
    }

    void AmbientOcclusion::configureResolveSampler(MaterialAssetView& material) const
    {
        for (const eastl::string& pipeline : material.getPipelineNames())
        {
            material.setSampler(pipeline, "ssaoLinearClamp", m_linearClamp, 6);
        }
    }

    bool AmbientOcclusion::createRandomTexture()
    {
        if (m_randomTexture)
        {
            return true;
        }
        constexpr uint32_t SIZE = d3d::mgpu::SSAO_RANDOM_TEXTURE_SIZE;
        m_randomTexture = d3d::create_tex(nullptr, SIZE, SIZE, TEXFMT_R8G8B8A8, 1, u8"ssao_random_vectors");
        void* mapped = nullptr;
        int stride = 0;
        if (!m_randomTexture || !m_randomTexture->lockimg(&mapped, stride, 0, TEXLOCK_WRITE) || !mapped)
        {
            if (m_randomTexture)
            {
                m_randomTexture->destroy();
                m_randomTexture = nullptr;
            }
            return false;
        }
        for (uint32_t y = 0; y < SIZE; ++y)
        {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + y * stride);
            for (uint32_t x = 0; x < SIZE; ++x)
            {
                row[x] = d3d::mgpu::ssaoRandomTexel(y * SIZE + x);
            }
        }
        m_randomTexture->unlockimg();
        return true;
    }
}  // namespace nau::render
