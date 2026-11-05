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
        constexpr float HBAO_TRACE_RADIUS = 2.f;

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

        uint32_t readTransportMode()
        {
            const auto transport = readSetting<eastl::string>("render/ssao/transport", "");
            if (transport == "mailbox") return 3;
            if (transport == "mailbox-cpu") return 2;
            if (transport == "framework") return 1;
            if (transport == "protected") return 0;
            return readSetting<bool>("render/ssao/frameworkTransport", true) ? 1u : 0u;
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
        const auto variant = static_cast<AmbientOcclusionVariant>(eastl::min(readSetting<uint32_t>("render/ssao/variant", 0), 3u));
        if (variant == AmbientOcclusionVariant::bogdanov)
        {
            settings.pipeline.inputs = AmbientOcclusionInputs::full;
            settings.pipeline.algorithm = AmbientOcclusionVariant::bogdanov;
            settings.pipeline.blur = AmbientOcclusionBlur::gaussian;
            settings.pipeline.upsample = AmbientOcclusionUpsample::bilinear;
        }
        else if (variant == AmbientOcclusionVariant::hbao)
        {
            settings.pipeline.algorithm = variant;
        }
        auto& p = settings.pipeline;
        p.algorithm = static_cast<AmbientOcclusionVariant>(eastl::min(readSetting<uint32_t>("render/ssao/algorithm", static_cast<uint32_t>(variant)), 3u));
        if (p.algorithm == AmbientOcclusionVariant::xegtao)
        {
            p.inputs = AmbientOcclusionInputs::full;
            p.fullResolution = true;
            p.blur = AmbientOcclusionBlur::off;
            p.upsample = AmbientOcclusionUpsample::bilinear;
        }
        p.inputs = static_cast<AmbientOcclusionInputs>(eastl::min(readSetting<uint32_t>("render/ssao/inputs", static_cast<uint32_t>(p.inputs)), 1u));
        p.normals = static_cast<AmbientOcclusionNormals>(eastl::min(readSetting<uint32_t>("render/ssao/normals", 0), 1u));
        p.blur = static_cast<AmbientOcclusionBlur>(eastl::min(readSetting<uint32_t>("render/ssao/blurMethod", readSetting<bool>("render/ssao/blur", true) ? static_cast<uint32_t>(p.blur) : 0u), 4u));
        p.blurExecution = readSetting<bool>("render/ssao/blurOnSecondary", readTransportMode() != 0) ? AmbientOcclusionExecution::multiGpu : AmbientOcclusionExecution::singleGpu;
        p.upsample = static_cast<AmbientOcclusionUpsample>(eastl::min(readSetting<uint32_t>("render/ssao/upsample", static_cast<uint32_t>(p.upsample)), 2u));
        p.packedNormals = readSetting<bool>("render/ssao/packedNormals", false);
        p.linearDepth16 = readSetting<bool>("render/ssao/linearDepth16", false);
        p.eightSamples = readSetting<bool>("render/ssao/eightSamples", false);
        p.weightedUpsample = readSetting<bool>("render/ssao/weightedUpsample", false);
        p.rejectOutside = readSetting<bool>("render/ssao/rejectOutside", false);
        p.quarterResolution = readSetting<bool>("render/ssao/quarterResolution", false);
        p.fullResolution = readSetting<bool>("render/ssao/fullResolution", p.fullResolution);
        p.gatherUpsample = readSetting<bool>("render/ssao/gatherUpsample", false);
        p.waitForSecondary = readTransportMode() == 0 && readSetting<bool>("render/ssao/waitForSecondary", true);
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
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Point;
        m_xePointClamp = d3d::create_sampler(sampler);
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
        const char* xeShaderPaths[] = {
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_prefilter.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_main.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.cslast]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_composite.cs.csmain]"
        };
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
                NAU_LOG_ERROR("XeGTAO: compute shader unavailable, {}", xeShaderPaths[index]);
        }
        // Load secondary shaders even when starting on GPU 0, so F8 can enable MGPU later.
        if (d3d::is_multi_gpu_available())
        {
            ShaderAssetRef vertex{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"};
            ShaderAssetRef rawShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            ShaderAssetRef filterShader{"file:/res/shaders/cache/shader_cache.nsbc+[ssao_blur.ps.pshalfblur]"};
            m_vertexShader = co_await vertex.getAssetViewTyped<ShaderAssetView>();
            m_rawShader = co_await rawShader.getAssetViewTyped<ShaderAssetView>();
            m_filterShader = co_await filterShader.getAssetViewTyped<ShaderAssetView>();
            // HBAO's compute path is optional; the raw pixel shader remains a fallback on either GPU.
            ShaderAssetRef hbaoShader{"file:/res/shaders/cache/shader_cache.nsbc+[hbao.cs.csmain]"};
            auto hbao = co_await hbaoShader.getAssetViewTyped<ShaderAssetView>().doTry();
            if (hbao) m_hbaoShader = *hbao;
            if (!m_vertexShader || !m_rawShader || !m_filterShader)
            {
                NAU_LOG_ERROR("SSAO: secondary shaders unavailable, using GPU 0");
            }
        }
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        resetSecondary();
        for (PROGRAM program : m_xePrograms)
            if (program != BAD_PROGRAM) d3d::delete_program(program);
        for (auto& timing : m_waitTimings)
        {
            d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &timing.begin, nullptr, nullptr);
            d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &timing.end, nullptr, nullptr);
            d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &timing.overlapStart, nullptr, nullptr);
        }
        for (auto& timing : m_filterTimings)
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
        ++m_timingGeneration;
        m_measurements.gpu1Timing = {};
        m_measurements.primaryFilterFrame = 0;
        m_measurements.primaryFilterMs = -1.f;
        m_pendingCompletions.clear();
        // Drain foreign queues before destroying any endpoint they may still reference.
        d3d::mgpu::destroySsaoProducer(m_secondary);
        m_secondary = nullptr;
        m_resolveSource = nullptr;
        m_shown = nullptr;
        m_frameworkOutputInitialized = false;
        m_drawRaw = m_drawDepth = m_drawNormals = nullptr;
        for (auto& job : m_jobs)
        {
            for (BaseTexture* texture : {job.normals, job.depth, job.result})
                if (texture) texture->destroy();
            job = {};
        }
        for (BaseTexture** texture : {&m_filtered, &m_horizontal, &m_fullOutput, &m_xeDepth, &m_xePing, &m_xePong, &m_xeEdges})
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
        m_xeAttempted = false;
        m_primaryWidth = 0;
        m_primaryHeight = 0;
        m_inputWidth = 0;
        m_inputHeight = 0;
        m_secondaryWidth = 0;
        m_secondaryHeight = 0;
    }

    void AmbientOcclusion::beginFrame(uint32_t width, uint32_t height, const Settings& settings)
    {
        const uint32_t transport = readTransportMode();
        const bool framework = transport == 1;
        const uint32_t mailbox = transport >= 2 ? transport - 1 : 0;
        if (framework != m_frameworkTransport || mailbox != m_mailboxMode)
        {
            resetSecondary();
            m_frameworkTransport = framework;
            m_mailboxMode = mailbox;
            ++m_timingGeneration;
            m_waitMilliseconds = -1.f;
            m_logSettings = true;
        }
        Settings effectiveSettings = settings;
        if (framework || mailbox) effectiveSettings.pipeline.waitForSecondary = false;
        const auto& old = m_settings.pipeline;
        const auto& next = effectiveSettings.pipeline;
        m_measureTimings = readSetting<bool>("render/measurements/enabled", false);
        d3d::mgpu::begin_foreign_queue_frame(readSetting<bool>("render/ssao/submitOnlyHandoffs", false), m_measureTimings);
        pollWaitTimings();
        pollFilterTimings();
        if (m_measureTimings)
        {
            m_measurements.gpu1Timing = d3d::mgpu::pollSsaoTiming(m_secondary);
            while (!m_pendingCompletions.empty() && d3d::mgpu::is_primary_transfer_complete(m_pendingCompletions.front().point))
            {
                const auto& completed = m_pendingCompletions.front();
                m_measurements.chainFrame = completed.frame;
                // This is an observed upper bound, sampled once per render frame, not a GPU copy duration.
                m_measurements.chainObservedMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - completed.submitted).count();
                m_pendingCompletions.pop_front();
            }
        }
        const bool asyncChanged = old.waitForSecondary != next.waitForSecondary ||
            ((!old.waitForSecondary || !next.waitForSecondary) &&
             (old != next || m_settings.execution != settings.execution ||
              (m_settings.mode == AmbientOcclusionMode::off) != (settings.mode == AmbientOcclusionMode::off)));
        if (old.waitForSecondary != next.waitForSecondary || m_settings.mode != settings.mode || m_settings.execution != settings.execution)
        {
            ++m_timingGeneration;
            m_waitMilliseconds = -1.f;
        }
        const bool rawPipelineChanged = old.algorithm != next.algorithm;
        const bool layoutChanged = rawPipelineChanged || old.inputs != next.inputs || old.normals != next.normals ||
            old.packedNormals != next.packedNormals || old.linearDepth16 != next.linearDepth16 ||
            old.resolutionDivisor() != next.resolutionDivisor();
        m_logSettings |= old != next || m_settings.execution != settings.execution || m_settings.mode != settings.mode;
        if (layoutChanged || asyncChanged) resetSecondary();
        m_settings = effectiveSettings;
        prepareTargets(width, height);
        // A full-size AO allocation exists only for a separate reconstruction pass.
        if (next.upsample != AmbientOcclusionUpsample::separate && m_fullOutput)
        {
            m_fullOutput->destroy();
            m_fullOutput = nullptr;
        }
        if (next.upsample == AmbientOcclusionUpsample::separate && !m_fullOutput && m_jobs[0].result)
            m_fullOutput = d3d::create_tex(nullptr, width, height, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_full_output");
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth)
    {
        m_measurements.frame = ++m_frameIndex;
        m_measurements.aoWidth = m_secondaryWidth;
        m_measurements.aoHeight = m_secondaryHeight;
        m_measurements.inputWidth = m_inputWidth;
        m_measurements.inputHeight = m_inputHeight;
        if (m_measureTimings)
        {
            m_measurements.phase = readSetting<uint32_t>("sample/automation/phase", 0);
            m_measurements.loadMask = readSetting<uint32_t>("sample/automation/activeLoadMask", 0);
        }
        m_measurements.gpu1 = m_measurements.aoValid = false;
        m_measurements.frameworkTransport = false;
        m_measurements.mailboxTransport = false;
        m_measurements.transportMode = m_mailboxMode ? m_mailboxMode + 1 : (m_frameworkTransport ? 1u : 0u);
        m_measurements.aoFrame = 0;
        m_currentTiming = nullptr;
        m_normals = normals;
        m_depth = depth;
        if (m_settings.pipeline.waitForSecondary) m_secondaryReady = {};
        m_usingSecondary = false;
        m_resolveSource = nullptr;
        m_drawRaw = m_jobs[0].result;
        m_drawDepth = m_jobs[0].depth;
        m_drawNormals = m_jobs[0].normals;
        const math::Matrix4 inverseProjection = inverse(projection);
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        const auto& p = m_settings.pipeline;
        const bool hbao = p.algorithm == AmbientOcclusionVariant::hbao;
        if (hbao) m_constants.parameters[0] = HBAO_TRACE_RADIUS;
        m_constants.filter[2] = m_blurDepthThreshold;
        if (m_settings.mode == AmbientOcclusionMode::off || !m_jobs[0].result)
        {
            logSettings();
            return;
        }
        // Keep the requested SSAO normal mode intact while HBAO always reads depth only.
        m_constants.options[0] = hbao || p.normals == AmbientOcclusionNormals::reconstructed ? 1.f : 0.f;
        m_constants.options[1] = p.packedNormals ? 1.f : 0.f;
        m_constants.options[2] = p.linearDepth16 ? 1.f : 0.f;
        m_constants.options[3] = hbao ? 16.f : (p.eightSamples ? 8.f : 14.f);
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
        m_constants.reconstruction[2] = static_cast<float>(p.resolutionDivisor());
        m_constants.reconstruction[3] = m_blurDepthThreshold;
        m_constants.filter[0] = static_cast<float>(m_secondaryWidth);
        m_constants.filter[1] = static_cast<float>(m_secondaryHeight);
        m_drawConstants = m_constants;
        if (m_settings.execution == AmbientOcclusionExecution::multiGpu && prepareSecondary())
        {
            if (m_frameworkTransport || m_mailboxMode)
            {
                m_measurements.aoFrame = m_measurements.chainFrame = m_measurements.timingFrame = 0;
                m_measurements.chainObservedMs = m_measurements.overlapMs = m_measurements.waitMs = -1.f;
                m_measurements.readyAtConsumer = -1;
                downsampleInputs(m_jobs[0].normals, m_jobs[0].depth);
                if (!m_frameworkOutputInitialized)
                {
                    d3d::set_render_target(m_jobs[0].result, 0);
                    d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
                    d3d::set_render_target();
                    m_frameworkOutputInitialized = true;
                }
                bool submitted = false;
                if (m_mailboxMode)
                    m_usingSecondary = d3d::mgpu::submitMailboxSsao(m_secondary, m_constants,
                        m_jobs[0].normals, m_jobs[0].depth, m_frameIndex, submitted);
                else
                    m_usingSecondary = d3d::mgpu::exchangeFrameworkSsao(m_secondary, m_constants,
                        m_jobs[0].normals, m_jobs[0].depth, m_jobs[0].result, m_frameIndex, submitted);
                if (m_usingSecondary)
                {
                    // Mailbox receives later, in render, after the independent G-buffer/CSM work.
                    // Its delayed readback identifies a previous consumed frame, never this CPU frame.
                    m_measurements.aoFrame = 0;
                    logSettings();
                    return;
                }
                d3d::mgpu::destroySsaoProducer(m_secondary);
                m_secondary = nullptr;
                m_logSettings = true;
            }
            else
            {
                AoJob* job = &m_jobs[0];
                if (!p.waitForSecondary)
                {
                    pollJobs();
                    job = findFreeJob();
                }
                m_usingSecondary = true;
                if (job)
                {
                    // Snapshot only a free job. Graphics queue order protects its previous readers.
                    downsampleInputs(job->normals, job->depth);
                    job->done = d3d::mgpu::submitSsao(m_secondary, m_constants, job->normals, job->depth, job->result, m_frameIndex);
                    m_usingSecondary = job->done.nativeFence != nullptr;
                    if (m_usingSecondary)
                    {
                        job->constants = m_constants;
                        job->frame = m_frameIndex;
                        if (!p.waitForSecondary) job->state = AoJob::State::InFlight;
                        m_secondaryReady = job->done;
                        if (m_measureTimings && m_pendingCompletions.size() < 128)
                            m_pendingCompletions.push_back({job->done, m_frameIndex, std::chrono::steady_clock::now()});
                    }
                    else
                    {
                        m_pendingCompletions.clear();
                        // Drain partial transfers before the primary fallback reuses any endpoint.
                        d3d::mgpu::destroySsaoProducer(m_secondary);
                        m_secondary = nullptr;
                        m_shown = nullptr;
                        for (auto& pending : m_jobs)
                        {
                            pending.state = AoJob::State::Free;
                            pending.done = {};
                        }
                        m_secondaryReady = {};
                        m_logSettings = true;
                    }
                }
            }
        }
        if (!m_usingSecondary) downsampleInputs(m_drawNormals, m_drawDepth);
        if (m_usingSecondary) beginOverlapTiming();
        logSettings();
    }

    void AmbientOcclusion::render(BaseTexture* output)
    {
        bool rawValid = true;
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        if (m_settings.mode == AmbientOcclusionMode::off || !m_jobs[0].result)
        {
            d3d::set_render_target(output, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            d3d::set_render_target();
            return;
        }
        if (m_usingSecondary)
        {
            m_measurements.gpu1 = true;
            m_measurements.frameworkTransport = m_frameworkTransport;
            m_measurements.mailboxTransport = m_mailboxMode != 0;
            if (!m_frameworkTransport && !m_mailboxMode && !m_settings.pipeline.waitForSecondary && m_currentTiming)
            {
                m_currentTiming->readyAtConsumer = d3d::mgpu::is_primary_transfer_complete(m_secondaryReady) ? 1 : 0;
                m_currentTiming->waited = false;
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &m_currentTiming->begin, nullptr, nullptr);
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &m_currentTiming->end, nullptr, nullptr);
            }
            // This wait is recorded after G-buffer/CSM, immediately before consuming this frame's AO.
            if (m_mailboxMode)
            {
                if (!d3d::mgpu::receiveMailboxSsao(m_secondary, m_jobs[0].result, m_frameIndex))
                {
                    // Drain before reusing endpoints after a partial native submission or device loss.
                    d3d::mgpu::destroySsaoProducer(m_secondary);
                    m_secondary = nullptr;
                    m_usingSecondary = false;
                    m_measurements.gpu1 = m_measurements.mailboxTransport = false;
                    m_measurements.gpu1Timing = {};
                    m_logSettings = true;
                    downsampleInputs(m_drawNormals, m_drawDepth);
                    rawValid = drawRaw();
                }
                m_drawConstants = m_constants;
            }
            else if (m_frameworkTransport)
            {
                // Shared AO was copied unconditionally in the primary graphics list.
                m_drawConstants = m_constants;
            }
            else if (m_settings.pipeline.waitForSecondary)
                waitForSecondary();
            else
            {
                // Catch transfers completed during the independent G-buffer/CSM work too.
                pollJobs();
                selectNewestReadyJob();
                if (!m_shown)
                {
                    d3d::set_render_target(output, 0);
                    d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
                    d3d::set_render_target();
                    return;
                }
                m_drawRaw = m_shown->result;
                m_drawDepth = m_shown->depth;
                m_drawNormals = m_shown->normals;
                m_drawConstants = m_shown->constants;
                m_measurements.aoFrame = m_shown->frame;
            }
        }
        else
        {
            // Match GPU 1's selected input size, shader and half-size R8 output.
            rawValid = drawRaw();
        }
        if (!m_measurements.aoFrame && !(m_usingSecondary && (m_frameworkTransport || m_mailboxMode))) m_measurements.aoFrame = m_frameIndex;
        m_measurements.aoValid = rawValid;
        filter(output);
    }

    bool AmbientOcclusion::drawRaw()
    {
        if (m_settings.pipeline.algorithm != AmbientOcclusionVariant::xegtao)
        {
            draw(m_drawRaw, *m_rawMaterial, "default");
            return true;
        }
        if (drawXeGtao()) return true;
        d3d::set_render_target(m_drawRaw, 0);
        d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
        d3d::set_render_target();
        return false;
    }

    bool AmbientOcclusion::drawXeGtao()
    {
        for (PROGRAM program : m_xePrograms)
            if (program == BAD_PROGRAM) return false;
        if (!m_xeDepth)
        {
            if (m_xeAttempted) return false;
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
                    if (*texture) (*texture)->destroy();
                    *texture = nullptr;
                }
                return false;
            }
            NAU_LOG_INFO("XeGTAO: GPU 0 AO path=compute, quality=high, denoise=2");
        }
        if (!m_xePing || !m_xePong || !m_xeEdges) return false;
        if (!m_frameworkOutputInitialized)
        {
            // Initialize render-target metadata before the first UAV write.
            d3d::set_render_target(m_drawRaw, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            m_frameworkOutputInitialized = true;
        }
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_cs_constbuffer_size(sizeof(m_drawConstants) / 16);
        d3d::set_cs_const(0, &m_drawConstants, sizeof(m_drawConstants) / 16);
        d3d::set_sampler(STAGE_CS, 0, m_xePointClamp);
        d3d::resource_barrier({m_drawDepth, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_drawNormals, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_tex(STAGE_CS, 0, m_drawNormals);
        d3d::set_tex(STAGE_CS, 1, m_drawDepth);

        d3d::resource_barrier({m_xeDepth, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[0]);
        for (uint32_t mip = 0; mip < 5; ++mip)
            d3d::set_rwtex(STAGE_CS, mip, m_xeDepth, 0, mip);
        d3d::dispatch((m_secondaryWidth + 15) / 16, (m_secondaryHeight + 15) / 16, 1);
        for (uint32_t slot = 0; slot < 5; ++slot)
            d3d::set_rwtex(STAGE_CS, slot, nullptr, 0, 0);
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
        d3d::resource_barrier({m_drawRaw, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[4]);
        d3d::set_tex(STAGE_CS, 2, source);
        d3d::set_rwtex(STAGE_CS, 0, m_drawRaw, 0, 0);
        d3d::dispatch((m_secondaryWidth + 7) / 8, (m_secondaryHeight + 7) / 8, 1);
        d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
        for (uint32_t slot = 0; slot < 4; ++slot)
            d3d::set_tex(STAGE_CS, slot, nullptr);
        d3d::resource_barrier({m_drawRaw, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        return true;
    }

    void AmbientOcclusion::pollJobs()
    {
        for (auto& job : m_jobs)
            if (job.state == AoJob::State::InFlight && d3d::mgpu::is_primary_transfer_complete(job.done))
                job.state = AoJob::State::Ready;
    }

    AmbientOcclusion::AoJob* AmbientOcclusion::findFreeJob()
    {
        size_t inFlight = 0;
        AoJob* free = nullptr;
        for (auto& job : m_jobs)
        {
            if (job.state == AoJob::State::InFlight) ++inFlight;
            if (!free && job.state == AoJob::State::Free) free = &job;
        }
        return inFlight < kMaxInFlight ? free : nullptr;
    }

    void AmbientOcclusion::selectNewestReadyJob()
    {
        AoJob* newest = nullptr;
        for (auto& job : m_jobs)
            if (job.state == AoJob::State::Ready && (!newest || job.frame > newest->frame))
                newest = &job;

        if (newest && (!m_shown || newest->frame > m_shown->frame))
        {
            // Later downsampling is on the same graphics queue as previous snapshot readers.
            // The result's return copy is ordered after its previous readers by the transfer API.
            if (m_shown) m_shown->state = AoJob::State::Free;
            m_shown = newest;
            m_shown->state = AoJob::State::Shown;
        }
        // Ready includes every input read and return copy. Older results can now be reused.
        for (auto& job : m_jobs)
            if (job.state == AoJob::State::Ready && m_shown && job.frame <= m_shown->frame)
                job.state = AoJob::State::Free;
    }

    bool AmbientOcclusion::prepareTargets(uint32_t width, uint32_t height)
    {
        TextureInfo info{};
        info.w = width;
        info.h = height;
        if (m_jobs[0].result && m_primaryWidth == info.w && m_primaryHeight == info.h)
        {
            return true;
        }
        resetSecondary();
        m_primaryWidth = info.w;
        m_primaryHeight = info.h;
        const uint32_t divisor = m_settings.pipeline.resolutionDivisor();
        m_secondaryWidth = (info.w + divisor - 1) / divisor;
        m_secondaryHeight = (info.h + divisor - 1) / divisor;
        const bool xegtao = m_settings.pipeline.algorithm == AmbientOcclusionVariant::xegtao;
        if (xegtao)
        {
            // Five depth mips require a minimum extent of sixteen texels.
            m_secondaryWidth = eastl::max(m_secondaryWidth, 16u);
            m_secondaryHeight = eastl::max(m_secondaryHeight, 16u);
        }
        m_inputWidth = m_settings.pipeline.inputs == AmbientOcclusionInputs::full ? info.w : m_secondaryWidth;
        m_inputHeight = m_settings.pipeline.inputs == AmbientOcclusionInputs::full ? info.h : m_secondaryHeight;
        if (!createRandomTexture())
        {
            return false;
        }
        const size_t jobCount = !m_frameworkTransport && !m_mailboxMode && m_settings.execution == AmbientOcclusionExecution::multiGpu && !m_settings.pipeline.waitForSecondary ? kJobCount : 1;
        for (size_t index = 0; index < jobCount; ++index)
        {
            auto& job = m_jobs[index];
            job.normals = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (m_settings.pipeline.packedNormals ? TEXFMT_R8G8 : TEXFMT_G16R16F) | TEXCF_RTARGET, 1, u8"ssao_job_normals");
            job.depth = d3d::create_tex(nullptr, m_inputWidth, m_inputHeight, (m_settings.pipeline.linearDepth16 ? TEXFMT_R16F : TEXFMT_R32F) | TEXCF_RTARGET, 1, u8"ssao_job_depth");
            job.result = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET | (xegtao ? TEXCF_UNORDERED : 0), 1, u8"ssao_job_result");
            if (!job.normals || !job.depth || !job.result)
            {
                NAU_LOG_ERROR("SSAO: could not allocate job targets");
                resetSecondary();
                return false;
            }
        }
        m_filtered = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_half_filtered");
        m_horizontal = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R16F | TEXCF_RTARGET, 1, u8"ssao_bogdanov_horizontal");
        if (!m_filtered || !m_horizontal)
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
            const bool hbao = m_settings.pipeline.algorithm == AmbientOcclusionVariant::hbao;
            const bool xegtao = m_settings.pipeline.algorithm == AmbientOcclusionVariant::xegtao;
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
            if (xegtao)
            {
                for (const auto& shader : m_xeShaders)
                    if (!shader || shader->getShader()->bytecode.empty())
                    {
                        NAU_LOG_ERROR("XeGTAO: compute shaders unavailable");
                        return false;
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
            desc.reconstructNormals = hbao || m_settings.pipeline.normals == AmbientOcclusionNormals::reconstructed;
            desc.packedNormals = m_settings.pipeline.packedNormals;
            desc.linearDepth16 = m_settings.pipeline.linearDepth16;
            desc.measureTimings = m_measureTimings;
            desc.frameworkStyle = m_frameworkTransport;
            desc.mailboxStyle = m_mailboxMode != 0;
            desc.gpuMailbox = m_mailboxMode == 2;
            const Shader* filter = m_filterShader->getShader();
            desc.filterBytecode = filter->bytecode.data();
            desc.filterBytecodeSize = filter->bytecode.size();
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (!m_secondary)
            {
                return false;
            }
            if (hbao && !desc.rawComputeBytecode)
                NAU_LOG_INFO("HBAO: GPU 1 raw AO path=pixel");
            ++m_measurements.gpu1Generation;
            NAU_LOG_INFO("AO: GPU 1 producer, AO {}x{}, algorithm={}, framework unsynchronized={}, mailbox mode={}",
                m_secondaryWidth, m_secondaryHeight, static_cast<uint32_t>(m_settings.pipeline.algorithm), m_frameworkTransport, m_mailboxMode);
        }
        if (!m_secondary)
        {
            return false;
        }
        return true;
    }

    void AmbientOcclusion::downsampleInputs(BaseTexture* normals, BaseTexture* depth)
    {
        d3d::resource_barrier({m_normals, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_depth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(normals, 0);
        d3d::set_render_target(1, depth, 0);
        d3d::setview(0, 0, m_inputWidth, m_inputHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
        setConstants(*m_blurMaterial, "downsample", m_constants);
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
            FilterTiming* sample = nullptr;
            if (m_measureTimings)
                for (auto& timing : m_filterTimings)
                    if (!timing.pending)
                    {
                        sample = &timing;
                        timing.frame = m_frameIndex;
                        timing.generation = m_timingGeneration;
                        timing.pending = true;
                        d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing.begin, nullptr, nullptr);
                        break;
                    }
            m_drawConstants.pipeline[1] = 0.f;
            draw(p.blur == AmbientOcclusionBlur::gaussian ? m_horizontal : m_filtered, *m_blurMaterial, "halfBlur", m_drawRaw);
            if (p.blur == AmbientOcclusionBlur::gaussian)
            {
                m_drawConstants.pipeline[1] = 1.f;
                draw(m_filtered, *m_blurMaterial, "halfBlur", m_horizontal);
            }
            m_resolveSource = m_filtered;
            if (sample) d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &sample->end, nullptr, nullptr);
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

    void AmbientOcclusion::setConstants(MaterialAssetView& material, const char* pipeline, const d3d::mgpu::SsaoConstants& constants)
    {
        const auto matrix = [](const float* values) { math::Matrix4 m; std::memcpy(&m, values, sizeof(m)); return m; };
        const auto vector = [](const float* v) { return math::Vector4(v[0], v[1], v[2], v[3]); };
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
        setConstants(material, pipeline, m_drawConstants);
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
        return math::Vector4(m_settings.mode != AmbientOcclusionMode::off && m_resolveSource ? 1.f : 0.f, m_fullOutput ? -1.f : m_drawConstants.reconstruction[0], m_settings.pipeline.linearDepth16 ? 1.f : 0.f, m_settings.pipeline.gatherUpsample ? 1.f : 0.f);
    }

    void AmbientOcclusion::pollFilterTimings()
    {
        uint64_t frequency = 0;
        d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &frequency, nullptr, nullptr);
        for (auto& timing : m_filterTimings)
        {
            uint64_t begin = 0, end = 0;
            if (!timing.pending ||
                !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.begin, &begin, nullptr) ||
                !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.end, &end, nullptr)) continue;
            if (frequency && timing.generation == m_timingGeneration && timing.frame > m_measurements.primaryFilterFrame && end >= begin)
            {
                m_measurements.primaryFilterFrame = timing.frame;
                m_measurements.primaryFilterMs = static_cast<float>(1000.0 * static_cast<double>(end - begin) / frequency);
            }
            timing.pending = false;
        }
    }

    void AmbientOcclusion::pollWaitTimings()
    {
        uint64_t frequency = 0;
        d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &frequency, nullptr, nullptr);
        for (auto& timing : m_waitTimings)
        {
            uint64_t begin = 0, end = 0, overlap = 0;
            if (timing.pending && d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.begin, &begin, nullptr) &&
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.end, &end, nullptr) &&
                (!m_measureTimings || !timing.overlapStart || d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, timing.overlapStart, &overlap, nullptr)))
            {
                if (timing.generation == m_timingGeneration && timing.sequence > m_lastWaitSequence && frequency && end >= begin)
                {
                    if (timing.waited) m_waitMilliseconds = static_cast<float>(1000.0 * static_cast<double>(end - begin) / frequency);
                    m_lastWaitSequence = timing.sequence;
                    m_measurements.timingFrame = timing.frame;
                    m_measurements.waitMs = timing.waited ? static_cast<float>(1000.0 * static_cast<double>(end - begin) / frequency) : -1.f;
                    m_measurements.overlapMs = timing.overlapStart && begin >= overlap ? static_cast<float>(1000.0 * static_cast<double>(begin - overlap) / frequency) : -1.f;
                    m_measurements.readyAtConsumer = timing.readyAtConsumer;
                }
                timing.pending = false;
            }
        }
    }

    void AmbientOcclusion::beginOverlapTiming()
    {
        if (!m_measureTimings) return;
        for (auto& timing : m_waitTimings)
        {
            if (timing.pending) continue;
            m_currentTiming = &timing;
            timing.pending = true;
            timing.frame = m_frameIndex;
            timing.waited = m_settings.pipeline.waitForSecondary;
            timing.readyAtConsumer = -1;
            timing.generation = m_timingGeneration;
            timing.sequence = ++m_nextWaitSequence;
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing.overlapStart, nullptr, nullptr);
            break;
        }
    }

    void AmbientOcclusion::waitForSecondary()
    {
        WaitTiming* sample = m_currentTiming;
        if (!sample)
            for (auto& timing : m_waitTimings)
                if (!timing.pending) { sample = &timing; break; }
        // Timestamp queries run on GPU 0 on either side of the graphics-queue Wait.
        // CPU-side duration of queue submission would not measure GPU idle time.
        if (sample)
        {
            sample->readyAtConsumer = d3d::mgpu::is_primary_transfer_complete(m_secondaryReady) ? 1 : 0;
            sample->frame = m_frameIndex;
            sample->waited = true;
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &sample->begin, nullptr, nullptr);
        }
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
        if (m_mailboxMode) return "OFF (mailbox)";
        if (m_frameworkTransport) return "OFF (framework, unsynchronized)";
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
            if (m_mailboxMode) return m_mailboxMode == 2 ? "MGPU mailbox GPU (2b)" : "MGPU mailbox CPU (2a)";
            if (m_frameworkTransport) return "MGPU framework (unsynchronized)";
            if (m_settings.pipeline.fullResolution) return "MGPU, full AO";
            return m_settings.pipeline.quarterResolution ? "MGPU, quarter AO" : "MGPU, half AO";
        }
        if (m_settings.pipeline.fullResolution)
            return m_settings.execution == AmbientOcclusionExecution::multiGpu ? "single GPU, full AO (MGPU unavailable)" : "single GPU, full AO";
        return m_settings.execution == AmbientOcclusionExecution::multiGpu
                   ? (m_settings.pipeline.quarterResolution ? "single GPU, quarter AO (MGPU unavailable)" : "single GPU, half AO (MGPU unavailable)")
                   : (m_settings.pipeline.quarterResolution ? "single GPU, quarter AO" : "single GPU, half AO");
    }

    const char* AmbientOcclusion::getVariantLabel() const
    {
        if (m_settings.pipeline.algorithm == AmbientOcclusionVariant::xegtao)
            return "XeGTAO / high / two denoise passes";
        if (m_settings.pipeline.algorithm == AmbientOcclusionVariant::hbao)
            return "HBAO / depth normals / independent stages";
        return m_settings.pipeline.algorithm == AmbientOcclusionVariant::bogdanov ? "Bogdanov AO / independent stages" : "Nau AO / independent stages";
    }

    void AmbientOcclusion::logSettings()
    {
        if (!m_logSettings) return;
        const auto& p = m_settings.pipeline;
        const bool hbao = p.algorithm == AmbientOcclusionVariant::hbao;
        const bool xegtao = p.algorithm == AmbientOcclusionVariant::xegtao;
        const auto normals = hbao ? AmbientOcclusionNormals::reconstructed : p.normals;
        NAU_LOG_INFO("AO pipeline: {}, AO={}x{}, inputs={}x{}, algorithm={}, radius={}, normals={}, requestedNormals={}, blur={}, blurGPU={}, upsample={}, RG8={}, R16F={}, {}={}, weighted={}, reject={}, gatherUp={}, wait={}, mode={}",
            getExecutionLabel(), m_secondaryWidth, m_secondaryHeight, m_inputWidth, m_inputHeight,
            static_cast<uint32_t>(p.algorithm), m_constants.parameters[0], static_cast<uint32_t>(normals), static_cast<uint32_t>(p.normals), static_cast<uint32_t>(p.blur),
            static_cast<uint32_t>(p.blurExecution), static_cast<uint32_t>(p.upsample), p.packedNormals, p.linearDepth16,
            xegtao ? "slices" : (hbao ? "directions" : "samples"), xegtao ? 3 : (hbao ? 16 : (p.eightSamples ? 8 : 14)), p.weightedUpsample, !hbao && !xegtao && p.rejectOutside, p.gatherUpsample, p.waitForSecondary, static_cast<uint32_t>(m_settings.mode));
        m_logSettings = false;
    }

    BaseTexture* AmbientOcclusion::getResolveTexture(BaseTexture* fallback) const
    {
        return m_resolveSource ? (m_fullOutput ? m_fullOutput : m_resolveSource) : fallback;
    }

    void AmbientOcclusion::bindResolveInputs()
    {
        if (BaseTexture* source = getResolveTexture(nullptr))
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::resource_barrier({m_drawDepth, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::settex(7, m_drawDepth);
        const math::Vector4 reconstruction(m_drawConstants.reconstruction[2], m_blurDepthThreshold,
            m_drawConstants.projection[14], m_drawConstants.projection[10]);
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
