// ambient_occlusion.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cstring>

#include "nau/3d/dag_drv3dCmd.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau/3d/ssao_random_texture.h"
#include "nau/app/global_properties.h"
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

    bool AmbientOcclusion::usesPrepassNormals() const
    {
        return readSetting<bool>("render/ssao/prepassNormals", false) || !readSetting<eastl::string>("render/ssao/xegtaoShaderCache", {}).empty();
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
        const eastl::string postPath = readSetting<eastl::string>("render/ssao/postMaterial", {});
        if (postPath.empty())
        {
            co_yield NauMakeError("AO requires a project postMaterial asset path");
        }
        MaterialAssetRef post{postPath};
        m_postMaterial = co_await post.getAssetViewTyped<MaterialAssetView>();
        if (!m_postMaterial)
        {
            co_yield NauMakeError("Failed to load the configured AO post material");
        }
        m_postMaterial->enableAutoSetTextures(false);
        d3d::SamplerInfo sampler;
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Linear;
        sampler.mip_map_mode = d3d::MipMapMode::Point;
        m_linearWrap = d3d::create_sampler(sampler);
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = d3d::AddressMode::Clamp;
        m_linearClamp = d3d::create_sampler(sampler);
        for (const auto id : {AmbientOcclusionAlgorithm::ssao, AmbientOcclusionAlgorithm::hbao, AmbientOcclusionAlgorithm::xegtao})
        {
            auto algorithm = ao::createAlgorithm(id);
            if (id == AmbientOcclusionAlgorithm::hbao && readSetting<eastl::string>("render/ssao/hbaoMaterial", {}).empty())
            {
                continue;
            }
            if (id == AmbientOcclusionAlgorithm::xegtao && readSetting<eastl::string>("render/ssao/xegtaoShaderCache", {}).empty())
            {
                continue;
            }
            const Result<> loaded = co_await algorithm->loadAssets().doTry();
            if (!loaded)
            {
                if (id == AmbientOcclusionAlgorithm::ssao)
                {
                    co_yield loaded.getError();
                }
                NAU_LOG_WARNING("{} unavailable: {}", algorithm->label(), loaded.getError()->getMessage());
                continue;
            }
            if (auto* material = algorithm->material())
            {
                material->setSampler("default", "aoLinearClamp", m_linearClamp, 0);
                material->setSampler("default", "aoLinearWrap", m_linearWrap, 1);
            }
            m_algorithms[static_cast<size_t>(id)] = std::move(algorithm);
        }
        for (const auto& pipeline : m_postMaterial->getPipelineNames())
        {
            m_postMaterial->setSampler(pipeline, "aoLinearClamp", m_linearClamp, 0);
            m_postMaterial->setSampler(pipeline, "aoLinearWrap", m_linearWrap, 1);
        }
        const eastl::string vertexPath = readSetting<eastl::string>("render/ssao/vertexShader", {});
        if (!vertexPath.empty())
        {
            ShaderAssetRef vertex{vertexPath};
            auto loaded = co_await vertex.getAssetViewTyped<ShaderAssetView>().doTry();
            if (loaded)
            {
                m_vertexShader = *loaded;
            }
        }
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        resetSecondary();
        if (m_randomTexture)
        {
            m_randomTexture->destroy();
        }
    }

    void AmbientOcclusion::resetSecondary()
    {
        // Release the producer before destroying surfaces that its queues may still use.
        nau::render::ao::destroySecondaryExecutor(m_secondary);
        m_secondary = nullptr;
        for (BaseTexture** texture : {&m_inputNormals, &m_inputDepth, &m_result})
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
        for (auto& algorithm : m_algorithms)
        {
            if (algorithm)
            {
                algorithm->reset();
            }
        }
        m_primaryWidth = 0;
        m_primaryHeight = 0;
        m_secondaryWidth = 0;
        m_secondaryHeight = 0;
    }

    void AmbientOcclusion::beginFrame(uint32_t width, uint32_t height, const Settings& requested)
    {
        Settings settings = requested;
        const size_t index = static_cast<size_t>(settings.algorithm);
        if (index >= m_algorithms.size() || !m_algorithms[index])
        {
            settings.algorithm = AmbientOcclusionAlgorithm::ssao;
        }
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
            m_measurements.gpu1Timing = nau::render::ao::pollSecondary(m_secondary);
        }
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth)
    {
        m_measurements.frame = ++m_frameIndex;
        m_measurements.aoWidth = m_measurements.inputWidth = m_secondaryWidth;
        m_measurements.aoHeight = m_measurements.inputHeight = m_secondaryHeight;
        m_measurements.reconstructNormals = algorithm().reconstructNormals();
        if (m_measureTimings)
        {
            m_measurements.phase = readSetting<uint32_t>("sample/automation/phase", 0);
        }
        m_measurements.gpu1 = m_measurements.aoValid = false;
        m_measurements.aoFrame = 0;
        m_normals = normals;
        m_depth = depth;
        m_usingSecondary = false;
        if (m_settings.mode == AmbientOcclusionMode::off || !m_result ||
            (!algorithm().reconstructNormals() && !m_normals))
        {
            return;
        }

        const math::Matrix4 inverseProjection = inverse(projection);
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        algorithm().configureConstants(m_constants);
        m_constants.options[0] = m_measurements.reconstructNormals ? 1.f : 0.f;
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
            m_usingSecondary = nau::render::ao::submitSecondary(m_secondary, m_constants, m_inputNormals, m_inputDepth, m_frameIndex);
            if (!m_usingSecondary)
            {
                nau::render::ao::destroySecondaryExecutor(m_secondary);
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

        if (m_settings.mode == AmbientOcclusionMode::off || !m_result ||
            (!algorithm().reconstructNormals() && !m_normals))
        {
            logSettings();
            return;
        }

        if (m_usingSecondary && !nau::render::ao::receiveSecondary(m_secondary, m_result, m_frameIndex))
        {
            // Drain partial work before falling back to local AO.
            nau::render::ao::destroySecondaryExecutor(m_secondary);
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
        if (auto* material = algorithm().material())
        {
            draw(m_result, *material, "default");
            return true;
        }
        if (algorithm().draw({m_secondaryWidth, m_secondaryHeight, m_inputNormals, m_inputDepth, m_result, m_constants}))
        {
            return true;
        }
        d3d::set_render_target(m_result, 0);
        d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
        d3d::set_render_target();
        return false;
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
        m_secondaryWidth = eastl::max(m_secondaryWidth, algorithm().minimumExtent());
        m_secondaryHeight = eastl::max(m_secondaryHeight, algorithm().minimumExtent());
        if (!createRandomTexture())
        {
            return false;
        }
        if (usesPrepassNormals())
        {
            m_inputNormals = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_G16R16F | TEXCF_RTARGET, 1, u8"ao_input_normals");
        }
        m_inputDepth = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ao_input_depth");
        m_result = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET | (algorithm().unorderedOutput() ? TEXCF_UNORDERED : 0), 1, u8"ao_result");
        if (!m_inputDepth || !m_result || (usesPrepassNormals() && !m_inputNormals))
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
        if (!d3d::is_multi_gpu_available() || !m_vertexShader)
        {
            return false;
        }
        if (!m_secondaryAttempted)
        {
            m_secondaryAttempted = true;
            const auto& vertex = m_vertexShader->getShader()->bytecode;
            ao::ExecutorDescription desc;
            desc.width = m_secondaryWidth;
            desc.height = m_secondaryHeight;
            desc.vertexBytecode = vertex.data();
            desc.vertexBytecodeSize = vertex.size();
            if (!algorithm().configureSecondary(desc))
            {
                return false;
            }
            desc.reconstructNormals = algorithm().reconstructNormals();
            desc.measureTimings = m_measureTimings;
            m_secondary = ao::createSecondaryExecutor(desc);
            if (!m_secondary)
            {
                return false;
            }
            ++m_measurements.gpu1Generation;
            NAU_LOG_INFO("AO: GPU 1 producer, AO {}x{}, algorithm={}, common MGPU mailbox",
                         m_secondaryWidth, m_secondaryHeight, getAlgorithmLabel());
        }
        return m_secondary != nullptr;
    }

    void AmbientOcclusion::downsampleInputs()
    {
        d3d::resource_barrier({m_depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        const char* pipeline = "downsample";
        if (m_normals && m_inputNormals)
        {
            d3d::resource_barrier({m_normals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            d3d::set_render_target(m_inputNormals, 0);
            d3d::set_render_target(1, m_inputDepth, 0);
            d3d::settex(0, m_normals);
            pipeline = "downsampleNormals";
        }
        else
        {
            d3d::set_render_target(m_inputDepth, 0);
        }
        d3d::setview(0, 0, m_secondaryWidth, m_secondaryHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(1, m_depth);
        setConstants(*m_postMaterial, pipeline, m_constants);
        m_postMaterial->bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::set_const_buffer(STAGE_PS, 0, nullptr);
        d3d::settex(0, nullptr);
        d3d::settex(1, nullptr);
        d3d::set_render_target();
    }

    void AmbientOcclusion::setConstants(MaterialAssetView& material, const char* pipeline, const nau::render::AoConstants& constants)
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
        d3d::settex(1, m_inputDepth);
        d3d::settex(2, source);
        d3d::settex(3, m_depth);
        d3d::settex(4, m_randomTexture);
        setConstants(material, pipeline, m_constants);
        material.bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::set_const_buffer(STAGE_PS, 0, nullptr);
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
        return algorithm().label();
    }

    void AmbientOcclusion::logSettings()
    {
        if (!m_logSettings)
        {
            return;
        }
        NAU_LOG_INFO("AO pipeline: {}, AO={}x{}, inputs={}x{}, algorithm={}, radius={}, normals={}, requestedNormals={}, blur=0, blurGPU=0, upsample=1, RG8=false, R16F=false, weighted=true, reject=false, gatherUp=true, wait=false, mode={}",
                     getExecutionLabel(), m_secondaryWidth, m_secondaryHeight, m_secondaryWidth, m_secondaryHeight,
                     static_cast<uint32_t>(m_settings.algorithm), m_constants.parameters[0], algorithm().reconstructNormals() ? 1 : 0,
                     algorithm().reconstructNormals() ? 1 : 0, static_cast<uint32_t>(m_settings.mode));
        m_logSettings = false;
    }

    BaseTexture* AmbientOcclusion::getResolveTexture(BaseTexture* fallback) const
    {
        return m_measurements.aoValid ? m_result : fallback;
    }

    void AmbientOcclusion::bindResolveInputs(BaseTexture* fallbackDepth)
    {
        if (m_measurements.aoValid)
        {
            d3d::resource_barrier({m_result, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
            d3d::resource_barrier({m_inputDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::settex(7, m_measurements.aoValid ? m_inputDepth : fallbackDepth);
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
        constexpr uint32_t SIZE = nau::render::SSAO_RANDOM_TEXTURE_SIZE;
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
                row[x] = nau::render::ssaoRandomTexel(y * SIZE + x);
            }
        }
        m_randomTexture->unlockimg();
        return true;
    }
}  // namespace nau::render
