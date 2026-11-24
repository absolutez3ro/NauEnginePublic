// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

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

        constexpr uint32_t AO_DIVISOR = 2;
        constexpr float DEPTH_THRESHOLD = 0.2f;

        bool reconstructNormals(AmbientOcclusionAlgorithm algorithm)
        {
            return algorithm != AmbientOcclusionAlgorithm::xegtao;
        }

        void clearAoTarget(BaseTexture* target)
        {
            d3d::set_render_target(target, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            d3d::set_render_target();
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
        const uint32_t algorithm = readSetting<uint32_t>("render/ssao/algorithm", 0);
        if (algorithm == static_cast<uint32_t>(AmbientOcclusionAlgorithm::hbao) ||
            algorithm == static_cast<uint32_t>(AmbientOcclusionAlgorithm::xegtao))
        {
            settings.algorithm = static_cast<AmbientOcclusionAlgorithm>(algorithm);
        }
        return settings;
    }

    async::Task<> AmbientOcclusion::loadAssets()
    {
        MaterialAssetRef raw{"file:/res/materials/ssao.nmat_json"};
        MaterialAssetRef post{"file:/res/materials/ssao_blur.nmat_json"};
        m_rawMaterial = co_await raw.getAssetViewTyped<MaterialAssetView>();
        if (!m_rawMaterial)
        {
            co_yield NauMakeError("Failed to load /res/materials/ssao.nmat_json");
        }
        m_postMaterial = co_await post.getAssetViewTyped<MaterialAssetView>();
        if (!m_postMaterial)
        {
            co_yield NauMakeError("Failed to load /res/materials/ssao_blur.nmat_json");
        }
        m_rawMaterial->enableAutoSetTextures(false);
        m_postMaterial->enableAutoSetTextures(false);
        d3d::SamplerInfo sampler;
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Linear;
        sampler.mip_map_mode = d3d::MipMapMode::Point;
        m_linearWrap = d3d::create_sampler(sampler);
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = d3d::AddressMode::Clamp;
        m_linearClamp = d3d::create_sampler(sampler);
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Point;
        m_xePointClamp = d3d::create_sampler(sampler);
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_postMaterial.get()})
        {
            for (const auto& pipeline : material->getPipelineNames())
            {
                material->setSampler(pipeline, "aoLinearClamp", m_linearClamp, 0);
                material->setSampler(pipeline, "aoLinearWrap", m_linearWrap, 1);
            }
        }

        const char* xeShaderPaths[] = {
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_prefilter.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_main.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.cslast]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_composite.cs.csmain]"};
        for (size_t index = 0; index < m_xeShaders.size(); ++index)
        {
            ShaderAssetRef shader{xeShaderPaths[index]};
            auto loaded = co_await shader.getAssetViewTyped<ShaderAssetView>().doTry();
            if (loaded)
            {
                m_xeShaders[index] = *loaded;
                m_xePrograms[index] = ShaderAssetView::makeShaderProgram(eastl::span<ShaderAssetView::Ptr>(&m_xeShaders[index], 1));
            }
            else
            {
                NAU_LOG_ERROR("XeGTAO: compute shader unavailable, {}", xeShaderPaths[index]);
            }
        }
        // Keep the secondary shaders ready for switching to Multi GPU.
        if (d3d::is_multi_gpu_available())
        {
            ShaderAssetRef vertex{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"};
            ShaderAssetRef rawShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            m_vertexShader = co_await vertex.getAssetViewTyped<ShaderAssetView>();
            m_rawShader = co_await rawShader.getAssetViewTyped<ShaderAssetView>();
            // HBAO's compute path is optional; the raw pixel shader remains a fallback on either GPU.
            ShaderAssetRef hbaoShader{"file:/res/shaders/cache/shader_cache.nsbc+[hbao.cs.csmain]"};
            auto hbao = co_await hbaoShader.getAssetViewTyped<ShaderAssetView>().doTry();
            if (hbao)
            {
                m_hbaoShader = *hbao;
            }
            if (!m_vertexShader || !m_rawShader)
            {
                NAU_LOG_ERROR("SSAO: secondary shaders unavailable, using GPU 0");
            }
        }
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        resetSecondary();
        for (PROGRAM program : m_xePrograms)
        {
            if (program != BAD_PROGRAM)
            {
                d3d::delete_program(program);
            }
        }
        if (m_randomTexture)
        {
            m_randomTexture->destroy();
        }
    }

    void AmbientOcclusion::resetSecondary()
    {
        // Release the producer before destroying surfaces that its queues may still use.
        d3d::mgpu::destroySsaoProducer(m_secondary);
        m_secondary = nullptr;
        for (BaseTexture** texture : {&m_inputNormals, &m_inputDepth, &m_result, &m_xeDepth, &m_xePing, &m_xePong, &m_xeEdges})
        {
            if (*texture)
            {
                (*texture)->destroy();
                *texture = nullptr;
            }
        }
        m_measurements.gpu1Timing = {};
        m_resultInitialized = false;
        m_usingSecondary = false;
        m_secondaryAttempted = false;
        m_xeAttempted = false;
        m_primaryWidth = 0;
        m_primaryHeight = 0;
        m_secondaryWidth = 0;
        m_secondaryHeight = 0;
    }

    void AmbientOcclusion::beginFrame(uint32_t width, uint32_t height, const Settings& settings)
    {
        if (m_settings.algorithm != settings.algorithm || m_settings.execution != settings.execution ||
            (m_settings.mode == AmbientOcclusionMode::off) != (settings.mode == AmbientOcclusionMode::off))
        {
            resetSecondary();
            m_logSettings = true;
        }
        m_settings = settings;
        m_measureTimings = readSetting<bool>("render/measurements/enabled", false);
        d3d::mgpu::begin_foreign_queue_frame(false, m_measureTimings);
        prepareTargets(width, height);
        if (m_measureTimings)
        {
            m_measurements.gpu1Timing = d3d::mgpu::pollSsaoTiming(m_secondary);
        }
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth)
    {
        m_measurements.frame = ++m_frameIndex;
        m_measurements.aoWidth = m_measurements.inputWidth = m_secondaryWidth;
        m_measurements.aoHeight = m_measurements.inputHeight = m_secondaryHeight;
        m_measurements.reconstructNormals = reconstructNormals(m_settings.algorithm);
        if (m_measureTimings)
        {
            m_measurements.phase = readSetting<uint32_t>("sample/automation/phase", 0);
        }
        m_measurements.gpu1 = m_measurements.aoValid = false;
        m_measurements.aoFrame = 0;
        m_normals = normals;
        m_depth = depth;
        m_usingSecondary = false;
        if (m_settings.mode == AmbientOcclusionMode::off || !m_result)
        {
            return;
        }

        const math::Matrix4 inverseProjection = inverse(projection);
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        const bool hbao = m_settings.algorithm == AmbientOcclusionAlgorithm::hbao;
        m_constants.parameters[0] = hbao ? 2.f : 0.5f;
        m_constants.options[0] = m_measurements.reconstructNormals ? 1.f : 0.f;
        m_constants.options[3] = hbao ? 16.f : 14.f;
        m_constants.pipeline[3] = static_cast<float>(m_settings.algorithm);
        m_constants.sizes[0] = m_constants.sizes[2] = static_cast<float>(m_secondaryWidth);
        m_constants.sizes[1] = m_constants.sizes[3] = static_cast<float>(m_secondaryHeight);
        m_constants.reconstruction[0] = 3.0f;
        m_constants.reconstruction[1] = 1.0f;
        m_constants.reconstruction[2] = static_cast<float>(AO_DIVISOR);
        m_constants.filter[0] = m_constants.sizes[0];
        m_constants.filter[1] = m_constants.sizes[1];

        downsampleInputs();
        if (m_settings.execution == AmbientOcclusionExecution::multiGpu && prepareSecondary())
        {
            if (!m_resultInitialized)
            {
                clearAoTarget(m_result);
                m_resultInitialized = true;
            }
            m_usingSecondary = d3d::mgpu::submitMailboxSsao(m_secondary, m_constants, m_inputNormals, m_inputDepth, m_frameIndex);
            if (!m_usingSecondary)
            {
                d3d::mgpu::destroySsaoProducer(m_secondary);
                m_secondary = nullptr;
                m_measurements.gpu1Timing = {};
                m_logSettings = true;
            }
        }
    }

    void AmbientOcclusion::render(BaseTexture* output)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        clearAoTarget(output);

        if (m_settings.mode == AmbientOcclusionMode::off || !m_result)
        {
            logSettings();
            return;
        }

        if (m_usingSecondary && !d3d::mgpu::receiveMailboxSsao(m_secondary, m_result, m_frameIndex))
        {
            // Drain partial work before falling back to local AO.
            d3d::mgpu::destroySsaoProducer(m_secondary);
            m_secondary = nullptr;
            m_usingSecondary = false;
            m_measurements.gpu1Timing = {};
            m_logSettings = true;
        }
        m_measurements.gpu1 = m_usingSecondary;
        m_measurements.aoValid = m_usingSecondary || drawRaw();
        if (!m_usingSecondary)
        {
            m_measurements.aoFrame = m_frameIndex;
        }

        logSettings();
    }

    bool AmbientOcclusion::drawRaw()
    {
        if (m_settings.algorithm != AmbientOcclusionAlgorithm::xegtao)
        {
            draw(m_result, *m_rawMaterial, "default");
            return true;
        }
        if (drawXeGtao())
        {
            return true;
        }
        d3d::set_render_target(m_result, 0);
        d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
        d3d::set_render_target();
        return false;
    }

    bool AmbientOcclusion::drawXeGtao()
    {
        for (PROGRAM program : m_xePrograms)
        {
            if (program == BAD_PROGRAM)
            {
                return false;
            }
        }
        if (!m_xeDepth)
        {
            if (m_xeAttempted)
            {
                return false;
            }
            m_xeAttempted = true;
            m_xeDepth = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R16F | TEXCF_UNORDERED, 5, u8"xegtao_depth_mips");
            m_xePing = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32UI | TEXCF_UNORDERED, 1, u8"xegtao_ao_ping");
            m_xePong = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32UI | TEXCF_UNORDERED, 1, u8"xegtao_ao_pong");
            m_xeEdges = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_UNORDERED, 1, u8"xegtao_edges");
            if (!m_xeDepth || !m_xePing || !m_xePong || !m_xeEdges)
            {
                NAU_LOG_ERROR("XeGTAO: could not allocate GPU 0 working textures");
                for (BaseTexture** texture : {&m_xeDepth, &m_xePing, &m_xePong, &m_xeEdges})
                {
                    if (*texture)
                    {
                        (*texture)->destroy();
                    }
                    *texture = nullptr;
                }
                return false;
            }
            NAU_LOG_INFO("XeGTAO: GPU 0 AO path=compute, quality=high, denoise=2");
        }
        if (!m_xePing || !m_xePong || !m_xeEdges)
        {
            return false;
        }
        if (!m_resultInitialized)
        {
            // Initialize render-target metadata before the first UAV write.
            d3d::set_render_target(m_result, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            m_resultInitialized = true;
        }
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_cs_constbuffer_size(sizeof(m_constants) / 16);
        d3d::set_cs_const(0, &m_constants, sizeof(m_constants) / 16);
        d3d::set_sampler(STAGE_CS, 0, m_xePointClamp);
        d3d::resource_barrier({m_inputDepth, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_inputNormals, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_tex(STAGE_CS, 0, m_inputNormals);
        d3d::set_tex(STAGE_CS, 1, m_inputDepth);

        d3d::resource_barrier({m_xeDepth, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[0]);
        for (uint32_t mip = 0; mip < 5; ++mip)
        {
            d3d::set_rwtex(STAGE_CS, mip, m_xeDepth, 0, mip);
        }
        d3d::dispatch((m_secondaryWidth + 15) / 16, (m_secondaryHeight + 15) / 16, 1);
        for (uint32_t slot = 0; slot < 5; ++slot)
        {
            d3d::set_rwtex(STAGE_CS, slot, nullptr, 0, 0);
        }
        d3d::resource_barrier({m_xeDepth, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});

        d3d::resource_barrier({m_xePing, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_xeEdges, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[1]);
        d3d::set_tex(STAGE_CS, 2, m_xeDepth);
        d3d::set_rwtex(STAGE_CS, 0, m_xePing, 0, 0);
        d3d::set_rwtex(STAGE_CS, 1, m_xeEdges, 0, 0);
        d3d::dispatch((m_secondaryWidth + 7) / 8, (m_secondaryHeight + 7) / 8, 1);
        d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
        d3d::set_rwtex(STAGE_CS, 1, nullptr, 0, 0);
        d3d::resource_barrier({m_xeEdges, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_tex(STAGE_CS, 3, m_xeEdges);

        BaseTexture* source = m_xePing;
        BaseTexture* destination = m_xePong;
        for (size_t pass = 2; pass < 4; ++pass)
        {
            d3d::set_tex(STAGE_CS, 2, nullptr);
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
            d3d::resource_barrier({destination, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
            d3d::set_program(m_xePrograms[pass]);
            d3d::set_tex(STAGE_CS, 2, source);
            d3d::set_rwtex(STAGE_CS, 0, destination, 0, 0);
            d3d::dispatch((m_secondaryWidth + 15) / 16, (m_secondaryHeight + 7) / 8, 1);
            d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
            eastl::swap(source, destination);
        }
        d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_result, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[4]);
        d3d::set_tex(STAGE_CS, 2, source);
        d3d::set_rwtex(STAGE_CS, 0, m_result, 0, 0);
        d3d::dispatch((m_secondaryWidth + 7) / 8, (m_secondaryHeight + 7) / 8, 1);
        d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
        for (uint32_t slot = 0; slot < 4; ++slot)
        {
            d3d::set_tex(STAGE_CS, slot, nullptr);
        }
        d3d::resource_barrier({m_result, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        return true;
    }

    bool AmbientOcclusion::prepareTargets(uint32_t width, uint32_t height)
    {
        if (m_result && m_primaryWidth == width && m_primaryHeight == height)
        {
            return true;
        }
        resetSecondary();
        if (!width || !height)
        {
            return false;
        }
        m_primaryWidth = width;
        m_primaryHeight = height;
        m_secondaryWidth = (width + AO_DIVISOR - 1) / AO_DIVISOR;
        m_secondaryHeight = (height + AO_DIVISOR - 1) / AO_DIVISOR;
        const bool xegtao = m_settings.algorithm == AmbientOcclusionAlgorithm::xegtao;
        if (xegtao)
        {
            // Five depth mips require a minimum extent of sixteen texels.
            m_secondaryWidth = eastl::max(m_secondaryWidth, 16u);
            m_secondaryHeight = eastl::max(m_secondaryHeight, 16u);
        }
        if (!createRandomTexture())
        {
            return false;
        }
        m_inputNormals = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_G16R16F | TEXCF_RTARGET, 1, u8"ao_input_normals");
        m_inputDepth = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ao_input_depth");
        m_result = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET | (xegtao ? TEXCF_UNORDERED : 0), 1, u8"ao_result");
        if (!m_inputNormals || !m_inputDepth || !m_result)
        {
            NAU_LOG_ERROR("AO: could not allocate half-resolution targets");
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
            const bool hbao = m_settings.algorithm == AmbientOcclusionAlgorithm::hbao;
            const bool xegtao = m_settings.algorithm == AmbientOcclusionAlgorithm::xegtao;
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* raw = m_rawShader->getShader();
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = m_secondaryWidth;
            desc.height = m_secondaryHeight;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            if (xegtao)
            {
                for (const auto& shader : m_xeShaders)
                {
                    if (!shader || shader->getShader()->bytecode.empty())
                    {
                        NAU_LOG_ERROR("XeGTAO: compute shaders unavailable");
                        return false;
                    }
                }
                const auto& prefilter = m_xeShaders[0]->getShader()->bytecode;
                desc.xePrefilterBytecode = prefilter.data();
                desc.xePrefilterBytecodeSize = prefilter.size();
                const auto& main = m_xeShaders[1]->getShader()->bytecode;
                desc.xeMainBytecode = main.data();
                desc.xeMainBytecodeSize = main.size();
                const auto& denoise = m_xeShaders[2]->getShader()->bytecode;
                desc.xeDenoiseBytecode = denoise.data();
                desc.xeDenoiseBytecodeSize = denoise.size();
                const auto& denoiseLast = m_xeShaders[3]->getShader()->bytecode;
                desc.xeDenoiseLastBytecode = denoiseLast.data();
                desc.xeDenoiseLastBytecodeSize = denoiseLast.size();
                const auto& composite = m_xeShaders[4]->getShader()->bytecode;
                desc.xeCompositeBytecode = composite.data();
                desc.xeCompositeBytecodeSize = composite.size();
            }
            if (hbao && m_hbaoShader && !m_hbaoShader->getShader()->bytecode.empty())
            {
                const Shader* compute = m_hbaoShader->getShader();
                desc.rawComputeBytecode = compute->bytecode.data();
                desc.rawComputeBytecodeSize = compute->bytecode.size();
            }
            else if (hbao)
            {
                NAU_LOG_WARNING("HBAO: secondary compute shader unavailable, using the GPU 1 pixel path");
            }
            desc.reconstructNormals = reconstructNormals(m_settings.algorithm);
            desc.measureTimings = m_measureTimings;
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (!m_secondary)
            {
                return false;
            }
            if (hbao && !desc.rawComputeBytecode)
            {
                NAU_LOG_INFO("HBAO: GPU 1 raw AO path=pixel");
            }
            ++m_measurements.gpu1Generation;
            NAU_LOG_INFO("AO: GPU 1 producer, AO {}x{}, algorithm={}, mailbox 2b",
                         m_secondaryWidth, m_secondaryHeight, getAlgorithmLabel());
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
        d3d::set_render_target(m_inputNormals, 0);
        d3d::set_render_target(1, m_inputDepth, 0);
        d3d::setview(0, 0, m_secondaryWidth, m_secondaryHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
        setConstants(*m_postMaterial, "downsample", m_constants);
        m_postMaterial->bindPipeline("downsample");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(0, nullptr);
        d3d::settex(1, nullptr);
        d3d::set_render_target();
    }

    void AmbientOcclusion::setConstants(MaterialAssetView& material, const char* pipeline, const d3d::mgpu::SsaoConstants& constants)
    {
        const auto matrix = [](const float* values)
        {
            math::Matrix4 m;
            std::memcpy(&m, values, sizeof(m));
            return m;
        };
        const auto vector = [](const float* v)
        {
            return math::Vector4(v[0], v[1], v[2], v[3]);
        };
        material.setProperty(pipeline, "aoProjection", matrix(constants.projection));
        material.setProperty(pipeline, "aoInverseProjection", matrix(constants.inverseProjection));
        material.setProperty(pipeline, "aoView", matrix(constants.view));
        material.setProperty(pipeline, "aoParameters", vector(constants.parameters));
        material.setProperty(pipeline, "aoFilter", vector(constants.filter));
        material.setProperty(pipeline, "aoOptions", vector(constants.options));
        material.setProperty(pipeline, "aoPipeline", vector(constants.pipeline));
        material.setProperty(pipeline, "aoSizes", vector(constants.sizes));
        material.setProperty(pipeline, "aoReconstruction", vector(constants.reconstruction));
    }

    void AmbientOcclusion::draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source)
    {
        d3d::resource_barrier({m_inputNormals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_inputDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        if (source)
        {
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_inputNormals);
        d3d::settex(1, m_inputDepth);
        d3d::settex(2, source);
        d3d::settex(3, m_depth);
        d3d::settex(4, m_randomTexture);
        setConstants(material, pipeline, m_constants);
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
        if (!m_measurements.aoValid)
        {
            clearAoTarget(target);
            return;
        }
        draw(target, *m_postMaterial, "debugBilinear", m_result);
    }

    bool AmbientOcclusion::isDebugView() const
    {
        return m_settings.mode == AmbientOcclusionMode::debug;
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_measurements.aoValid ? 1.f : 0.f, 3.f, 0.f, 1.f);
    }

    const char* AmbientOcclusion::getExecutionLabel() const
    {
        if (m_settings.mode == AmbientOcclusionMode::off)
        {
            return "AO off";
        }
        if (m_usingSecondary)
        {
            return "MGPU mailbox GPU (2b)";
        }
        return m_settings.execution == AmbientOcclusionExecution::multiGpu
            ? "single GPU, half AO (MGPU unavailable)"
            : "single GPU, half AO";
    }

    const char* AmbientOcclusion::getAlgorithmLabel() const
    {
        switch (m_settings.algorithm)
        {
            case AmbientOcclusionAlgorithm::hbao:
                return "HBAO";
            case AmbientOcclusionAlgorithm::xegtao:
                return "XeGTAO";
            default:
                return "SSAO";
        }
    }

    void AmbientOcclusion::logSettings()
    {
        if (!m_logSettings)
        {
            return;
        }
        NAU_LOG_INFO("AO pipeline: {}, AO={}x{}, inputs={}x{}, algorithm={}, radius={}, normals={}, requestedNormals={}, blur=0, blurGPU=0, upsample=1, RG8=false, R16F=false, weighted=true, reject=false, gatherUp=true, wait=false, mode={}",
                     getExecutionLabel(), m_secondaryWidth, m_secondaryHeight, m_secondaryWidth, m_secondaryHeight,
                     static_cast<uint32_t>(m_settings.algorithm), m_constants.parameters[0], reconstructNormals(m_settings.algorithm) ? 1 : 0,
                     reconstructNormals(m_settings.algorithm) ? 1 : 0, static_cast<uint32_t>(m_settings.mode));
        m_logSettings = false;
    }

    BaseTexture* AmbientOcclusion::getResolveTexture(BaseTexture* fallback) const
    {
        return m_measurements.aoValid ? m_result : fallback;
    }

    void AmbientOcclusion::bindResolveInputs()
    {
        if (m_measurements.aoValid)
        {
            d3d::resource_barrier({m_result, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            d3d::resource_barrier({m_inputDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::settex(7, m_measurements.aoValid ? m_inputDepth : nullptr);
        const math::Vector4 reconstruction(static_cast<float>(AO_DIVISOR), DEPTH_THRESHOLD,
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
