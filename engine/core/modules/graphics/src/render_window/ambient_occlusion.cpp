// ambient_occlusion.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "ambient_occlusion_profiler.h"
#include "gpu_profiler/gpu_profiler.h"
#include "nau/app/global_properties.h"
#include "nau/assets/asset_manager.h"
#include "nau/diag/logging.h"
#include "nau/service/service_provider.h"

namespace nau::render
{
    namespace
    {
        constexpr UINT TITLE_MESSAGE = WM_APP + 0x4A0;
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

        float decodeHalf(uint16_t bits)
        {
            const uint32_t exponent = (bits >> 10) & 31u;
            const uint32_t mantissa = bits & 1023u;
            const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
            if (exponent == 31)
            {
                return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
            }
            return sign * std::ldexp(static_cast<float>(exponent != 0 ? 1024u + mantissa : mantissa),
                                     exponent != 0 ? static_cast<int>(exponent) - 25 : -24);
        }
    }  // namespace

    void AmbientOcclusionInput::setTarget(void* windowHandle)
    {
        const std::lock_guard<std::mutex> lock(m_titleMutex);
        m_windowHandle.store(windowHandle);
        m_switchRequests.store(0);
        m_deviceSwitchRequests.store(0);
        m_pendingTitle.clear();
        m_titlePosted = false;
    }

    void AmbientOcclusionInput::postTitle(eastl::string title)
    {
        const std::lock_guard<std::mutex> lock(m_titleMutex);
        HWND window = static_cast<HWND>(m_windowHandle.load());
        if (!window)
        {
            return;
        }
        m_pendingTitle = eastl::move(title);
        if (!m_titlePosted)
        {
            m_titlePosted = PostMessageW(window, TITLE_MESSAGE, 0, 0) != FALSE;
        }
    }

    uint32_t AmbientOcclusionInput::consumeSwitches()
    {
        return m_switchRequests.exchange(0);
    }

    uint32_t AmbientOcclusionInput::consumeDeviceSwitches()
    {
        return m_deviceSwitchRequests.exchange(0);
    }

    bool AmbientOcclusionInput::handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM)
    {
        if (hwnd == m_windowHandle.load() && message == TITLE_MESSAGE)
        {
            eastl::string title;
            {
                const std::lock_guard<std::mutex> lock(m_titleMutex);
                title.swap(m_pendingTitle);
                m_titlePosted = false;
            }
            if (!title.empty())
            {
                SetWindowTextA(hwnd, title.c_str());
            }
            return true;
        }
        if (hwnd == m_windowHandle.load() && message == WM_KEYUP && wParam == VK_F7)
        {
            m_switchRequests.fetch_add(1);
            return true;
        }
        if (hwnd == m_windowHandle.load() && message == WM_KEYUP && wParam == VK_F8)
        {
            m_deviceSwitchRequests.fetch_add(1);
            return true;
        }
        return false;
    }

    bool AmbientOcclusion::isRequested()
    {
        return readSetting<bool>("sample/ssao/enabled", false);
    }

    async::Task<> AmbientOcclusion::loadAssets()
    {
        MaterialAssetRef raw{"file:/res/materials/ssao.nmat_json"};
        MaterialAssetRef blur{"file:/res/materials/ssao_blur.nmat_json"};
        m_rawMaterial = co_await raw.getAssetViewTyped<MaterialAssetView>();
        m_blurMaterial = co_await blur.getAssetViewTyped<MaterialAssetView>();
        m_rawMaterial->enableAutoSetTextures(false);
        m_blurMaterial->enableAutoSetTextures(false);
        IAssetManager& assets = getServiceProvider().get<IAssetManager>();
        IAssetDescriptor::Ptr vertex = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.vs.vsmain]"});
        IAssetDescriptor::Ptr rawShader = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"});
        IAssetDescriptor::Ptr blurShader = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[ssao_blur.ps.psmain]"});
        IAssetDescriptor::Ptr normalShader = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[ssao_blur.ps.psreconstructnormals]"});
        m_vertexShader = co_await vertex->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_rawShader = co_await rawShader->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_blurShader = co_await blurShader->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_normalShader = co_await normalShader->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        const float radius = readFiniteSetting("sample/ssao/radius", 0.5f, 0.0f, 10.0f);
        const float fadeStart = readFiniteSetting("sample/ssao/fadeStart", 0.2f, 0.0f, 10.0f);
        const float fadeEnd = readFiniteSetting("sample/ssao/fadeEnd", 1.0f, fadeStart + 0.001f, 20.0f);
        const float epsilon = readFiniteSetting("sample/ssao/surfaceEpsilon", 0.05f, 0.0001f, fadeEnd);
        m_parameters = math::Vector4(radius, fadeStart, fadeEnd, epsilon);
        m_blurDepthThreshold = readFiniteSetting("sample/ssao/blurDepthThreshold", 0.2f, 0.0f, 10.0f);
        m_mode = eastl::min(readSetting<uint32_t>("sample/ssao/mode", 1), 2u);
        m_blur = readSetting<bool>("sample/ssao/blur", true);
        m_resolutionDivisor = readSetting<uint32_t>("sample/ssao/resolutionDivisor", 2) == 1 ? 1u : 2u;
        m_captureAtFrame = readSetting<uint32_t>("sample/ssao/captureAtFrame", 0);
        m_autoSwitchFrames = readSetting<uint32_t>("sample/ssao/autoSwitchFrames", 0);
        m_capturePrefix = readSetting<eastl::string>("sample/ssao/capturePrefix", "ssao");
        m_requestedSecondary = readSetting<bool>("sample/ssao/useSecondary", true);
        m_validateSecondary = readSetting<bool>("sample/ssao/validateSecondary", false);
        m_traceSubmissions = readSetting<bool>("sample/ssao/traceSubmissions", false);
        m_warpForTesting = readSetting<bool>("dx12/secondaryWarpForTesting", false);
        m_autoDeviceSwitchFrames = readSetting<uint32_t>("sample/ssao/autoDeviceSwitchFrames", 0);
        m_failAtFrame = readSetting<uint32_t>("sample/ssao/failAtFrame", 0);
        m_busyAtFrame = readSetting<uint32_t>("sample/ssao/busyAtFrame", 0);
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        if (m_initialized)
        {
            getServiceProvider().get<AmbientOcclusionInput>().setTarget(nullptr);
        }
        d3d::mgpu::destroySsaoProducer(m_secondary);
        if (m_history)
        {
            m_history->destroy();
        }
        if (m_receiving)
        {
            m_receiving->destroy();
        }
        if (m_historyDepth)
        {
            m_historyDepth->destroy();
        }
        if (m_pendingDepth)
        {
            m_pendingDepth->destroy();
        }
        NAU_LOG_INFO("SSAO: shutdown, frames={}, secondary={}, primary={}, busy={}, reused={}, submitted={}, maxAge={}, resubmitted={}", m_frame, m_secondaryFrames, m_primaryFrames, m_busyFrames, m_reusedFrames, m_submittedFrames, m_maxHistoryAge, m_resubmittedFrames);
    }

    void AmbientOcclusion::beginFrame(const math::Matrix4& view, const math::Matrix4& projection, void* windowHandle)
    {
        AmbientOcclusionInput& input = getServiceProvider().get<AmbientOcclusionInput>();
        m_windowHandle = windowHandle;
        if (!m_initialized)
        {
            input.setTarget(windowHandle);
            m_initialized = true;
            NAU_LOG_INFO("SSAO: 14 samples, bilateral blur={}, resolutionDivisor={}, nonblocking late receive, F7: off / lit / AO, F8: GPU 0 / GPU 1", m_blur, m_resolutionDivisor);
        }
        ++m_frame;
        uint32_t deviceSwitches = input.consumeDeviceSwitches();
        if (m_autoDeviceSwitchFrames != 0 && m_frame % m_autoDeviceSwitchFrames == 0)
        {
            ++deviceSwitches;
        }
        if ((deviceSwitches & 1u) != 0)
        {
            m_requestedSecondary = !m_requestedSecondary;
            m_historyFrame = 0;
            m_discardPending = true;
        }
        if (!m_failureInjected && m_failAtFrame != 0 && m_frame >= m_failAtFrame && m_secondaryFrames != 0)
        {
            m_failureInjected = true;
            NAU_LOG_WARNING("SSAO: injecting secondary failure at frame {}", m_frame);
            d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
        }
        uint32_t switches = input.consumeSwitches();
        if (m_autoSwitchFrames != 0 && m_frame % m_autoSwitchFrames == 0)
        {
            ++switches;
        }
        m_mode = (m_mode + switches) % 3;
        if (switches != 0)
        {
            m_historyFrame = 0;
            m_discardPending = true;
        }
        if (switches != 0 || m_frame == 1)
        {
            NAU_LOG_INFO("SSAO: mode={}, frame={}", m_mode, m_frame);
        }
        m_rawMaterial->setProperty("default", "aoProjection", projection);
        m_rawMaterial->setProperty("default", "aoParameters", m_parameters);
        const math::Matrix4 inverseProjection = inverse(projection);
        static_assert(sizeof(math::Matrix4) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        m_constants.filter[2] = m_blurDepthThreshold;
        m_constants.filter[3] = m_mode != 0 ? 1.0f : 0.0f;
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_blurMaterial.get()})
        {
            material->setProperty("default", "aoInverseProjection", inverseProjection);
            material->setProperty("default", "aoView", view);
        }
        m_blurMaterial->setProperty("reconstructNormals", "aoInverseProjection", inverseProjection);
        m_blurMaterial->setProperty("reconstructNormals", "aoView", view);
        m_blurMaterial->setProperty("upsample", "aoInverseProjection", inverseProjection);
    }

    void AmbientOcclusion::prepareSecondary(BaseTexture* target)
    {
        TextureInfo info;
        target->getinfo(info);
        if (m_secondaryWidth != info.w || m_secondaryHeight != info.h)
        {
            if (m_traceSubmissions && m_pendingFrame != 0)
            {
                NAU_LOG_INFO("SSAO: reset pendingSource={}", m_pendingFrame);
            }
            if (m_history)
            {
                m_history->destroy();
                m_history = nullptr;
            }
            m_historyFrame = 0;
            if (m_receiving)
            {
                m_receiving->destroy();
                m_receiving = nullptr;
            }
            if (m_historyDepth)
            {
                m_historyDepth->destroy();
                m_historyDepth = nullptr;
            }
            if (m_pendingDepth)
            {
                m_pendingDepth->destroy();
                m_pendingDepth = nullptr;
            }
            m_pendingFrame = 0;
            m_pendingCapture = false;
            m_captureSubmitted = false;
            d3d::mgpu::destroySsaoProducer(m_secondary);
            m_secondary = nullptr;
            m_secondaryAttempted = false;
            m_secondaryWidth = info.w;
            m_secondaryHeight = info.h;
        }
        if (!m_secondaryAttempted && m_requestedSecondary && d3d::is_multi_gpu_available())
        {
            m_secondaryAttempted = true;
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* raw = m_rawShader->getShader();
            const Shader* blur = m_blurShader->getShader();
            const Shader* normal = m_normalShader->getShader();
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = info.w;
            desc.height = info.h;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            desc.blurBytecode = blur->bytecode.data();
            desc.blurBytecodeSize = blur->bytecode.size();
            desc.profiler = createSsaoProfiler();
            desc.normalBytecode = normal->bytecode.data();
            desc.normalBytecodeSize = normal->bytecode.size();
            desc.depthIsFloat = true;
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (m_secondary)
            {
                m_history = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_history");
                m_receiving = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_receiving");
                m_historyDepth = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_history_depth");
                m_pendingDepth = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_pending_depth");
                if (!m_history || !m_receiving || !m_historyDepth || !m_pendingDepth)
                {
                    d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_OUTOFMEMORY));
                }
            }
            NAU_LOG_INFO("SSAO: secondary pass ready={}, size={}x{}", m_secondary != nullptr, info.w, info.h);
        }
    }

    void AmbientOcclusion::updateDeviceStatus(bool secondary, bool reused)
    {
        eastl::string status = secondary ? (m_warpForTesting ? "GPU 1 WARP -> GPU 0 (TEST, pipelined)" : "GPU 1 -> GPU 0 (pipelined)") : "GPU 0";
        if (reused)
        {
            status += " | cached AO";
        }
        if (m_requestedSecondary && !d3d::is_multi_gpu_available())
        {
            status += " | ";
            status += d3d::get_multi_gpu_fallback_reason();
        }
        const bool statusChanged = status != m_deviceStatus;
        if (statusChanged)
        {
            m_deviceStatus = status;
            NAU_LOG_INFO("SSAO: device={}, frame={}", status.c_str(), m_frame);
        }
        const uint64_t now = GetTickCount64();
        if (m_titleStartTicks == 0)
        {
            m_titleStartTicks = now;
        }
        else
        {
            ++m_titleFrames;
        }
        const uint64_t elapsed = now - m_titleStartTicks;
        const bool refreshTimings = elapsed >= 1000 && m_titleFrames != 0;
        if (refreshTimings)
        {
            m_titleFps = 1000.0 * m_titleFrames / elapsed;
            m_titleFrameMilliseconds = static_cast<double>(elapsed) / m_titleFrames;
            m_titleFrames = 0;
            m_titleStartTicks = now;
        }
        if (m_titleMode == 3 || refreshTimings)
        {
            constexpr const char* MODES[] = {"off", "lit", "map"};
            char title[256] = {};
            std::snprintf(title, sizeof(title), "SSAO %s | %s | %.1f FPS | %.2f ms/frame | F7: view | F8: GPU",
                          MODES[m_mode], status.c_str(), m_titleFps, m_titleFrameMilliseconds);
            getServiceProvider().get<AmbientOcclusionInput>().postTitle(title);
            m_titleMode = m_mode;
        }
    }

    uint32_t AmbientOcclusion::getResolutionDivisor() const
    {
        return m_resolutionDivisor;
    }

    void AmbientOcclusion::submitFrame(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* filtered, BaseTexture* depth, BaseTexture* packedNormals, BaseTexture* reducedDepth, BaseTexture* depthSnapshot)
    {
        m_sourceNormals = packedNormals;
        m_fullDepth = depthSnapshot;
        m_depth = reducedDepth;
        TextureInfo size;
        depth->getinfo(size);
        if (m_fullWidth != size.w || m_fullHeight != size.h)
        {
            m_fullWidth = size.w;
            m_fullHeight = size.h;
            m_historyFrame = 0;
            m_discardPending = true;
            m_captureSubmitted = false;
            TextureInfo aoSize;
            filtered->getinfo(aoSize);
            NAU_LOG_INFO("SSAO: resolution full={}x{}, ao={}x{}", size.w, size.h, aoSize.w, aoSize.h);
        }
        prepareSecondary(filtered);
        {
            NAU_GPU_TIMER("ssao_snapshot_depth");
            d3d::settex(1, depth);
            drawPipeline(depthSnapshot, "copyDepth");
        }
        {
            NAU_GPU_TIMER("ssao_reduce_depth");
            m_blurMaterial->setProperty("downsampleDepth", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, static_cast<float>(m_resolutionDivisor)));
            d3d::settex(3, depthSnapshot);
            drawPipeline(reducedDepth, "downsampleDepth");
        }
        m_trySecondary = m_requestedSecondary && m_mode != 0 && m_secondary && d3d::is_multi_gpu_available();
        m_forcedBusy = m_trySecondary && !m_busyInjected && m_busyAtFrame != 0 && m_frame >= m_busyAtFrame &&
                       (m_busyAtFrame == 1 || m_historyFrame != 0);
        m_captureRequested = m_captureAtFrame != 0 && m_frame >= m_captureAtFrame && !m_captureSubmitted;
        if (m_forcedBusy)
        {
            m_busyInjected = true;
            NAU_LOG_INFO("SSAO: injecting busy at frame {}", m_frame);
        }
        const bool canSubmit = m_trySecondary && !m_forcedBusy && m_pendingFrame == 0 && !d3d::mgpu::isSsaoBusy(m_secondary);
        const bool useHistory = m_trySecondary && m_historyFrame != 0;
        m_preparedNormals = !useHistory || (m_captureRequested && (canSubmit || m_forcedBusy));
        if (m_preparedNormals)
        {
            reconstructNormals(packedNormals);
            m_normals = packedNormals;
        }
        if (canSubmit)
        {
            trySubmitSecondary(raw, horizontal, filtered, false);
        }
        // Reduction/reference passes use a smaller viewport; G-buffer still renders at the full scene size.
        d3d::setview(0, 0, m_fullWidth, m_fullHeight, 0.0f, 1.0f);
    }

    bool AmbientOcclusion::trySubmitSecondary(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* filtered, bool afterReceive)
    {
        if (!m_trySecondary || m_forcedBusy || m_pendingFrame != 0 || m_lastSubmittedFrame == m_frame || d3d::mgpu::isSsaoBusy(m_secondary))
        {
            return false;
        }
        m_captureRequested = m_captureAtFrame != 0 && m_frame >= m_captureAtFrame && !m_captureSubmitted;
        const bool captureJob = m_captureRequested && (m_busyAtFrame == 0 || m_busyAtFrame == 1);
        if (captureJob)
        {
            if (!m_preparedNormals)
            {
                reconstructNormals(m_sourceNormals);
                m_normals = m_sourceNormals;
                m_preparedNormals = true;
            }
            m_captureActive = true;
            captureDepth(m_fullDepth, "source_full_depth");
            capture(m_sourceNormals, "normals", 4);
            captureDepth();
            if (m_validateSecondary)
            {
                drawRaw(raw, m_sourceNormals, m_depth);
                capture(raw, "reference_raw", 1);
                drawBlur(horizontal, raw, true);
                TextureInfo info;
                filtered->getinfo(info);
                BaseTexture* reference = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_reference");
                if (reference)
                {
                    drawBlur(reference, horizontal, false);
                    capture(reference, "reference", 1);
                    reference->destroy();
                }
                else
                {
                    NAU_LOG_ERROR("SSAO: could not allocate validation reference");
                }
            }
            m_captureActive = false;
        }
        // Retain the exact input until its AO arrives; the graph depth is overwritten next frame.
        if (!m_pendingDepth->update(m_depth))
        {
            d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
            m_trySecondary = false;
            m_historyFrame = 0;
            return false;
        }
        const d3d::mgpu::SsaoResult result = d3d::mgpu::submitSsao(m_secondary, m_constants, m_blur, nullptr, m_pendingDepth, captureJob);
        if (result == d3d::mgpu::SsaoResult::submitted)
        {
            m_pendingFrame = m_frame;
            m_pendingCapture = captureJob;
            m_discardPending = false;
            m_captureSubmitted |= captureJob;
            ++m_submittedFrames;
            m_lastSubmittedFrame = m_frame;
            if (afterReceive)
            {
                ++m_resubmittedFrames;
            }
            if (m_traceSubmissions)
            {
                NAU_LOG_INFO("SSAO: submitted frame={}, stage={}", m_frame, afterReceive ? "receive" : "submit");
            }
            return true;
        }
        else if (result == d3d::mgpu::SsaoResult::failed)
        {
            m_trySecondary = false;
            m_historyFrame = 0;
        }
        return false;
    }

    void AmbientOcclusion::resolveFrame(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* filtered, BaseTexture* output)
    {
        bool fresh = false;
        // Poll once before lighting; unfinished secondary work never stalls the primary queue.
        if (m_trySecondary && !m_forcedBusy && m_pendingFrame != 0)
        {
            const d3d::mgpu::SsaoResult result = d3d::mgpu::tryReceiveSsao(m_secondary, m_receiving, m_pendingCapture ? raw : nullptr);
            if (result == d3d::mgpu::SsaoResult::submitted)
            {
                if (m_traceSubmissions)
                {
                    NAU_LOG_INFO("SSAO: received frame={}, source={}, discarded={}", m_frame, m_pendingFrame, m_discardPending);
                }
                if (!m_discardPending)
                {
                    eastl::swap(m_history, m_receiving);
                    eastl::swap(m_historyDepth, m_pendingDepth);
                    m_historyFrame = m_pendingFrame;
                    fresh = true;
                    if (m_pendingCapture)
                    {
                        m_captureActive = true;
                        capture(raw, "raw", 1);
                        capture(m_history, "filtered", 1);
                        captureDepth(m_historyDepth, "history_depth");
                        m_captureActive = false;
                        m_captureDisplayFrame = m_frame;
                        NAU_LOG_INFO("SSAO: secondary capture frame={}, source={}", m_frame, m_historyFrame);
                    }
                }
                else if (m_pendingCapture)
                {
                    m_captureSubmitted = false;
                }
                m_pendingFrame = 0;
                m_pendingCapture = false;
                m_discardPending = false;
                // Refill immediately from this frame's retained depth, without waiting for the next submit node.
                trySubmitSecondary(raw, horizontal, filtered, true);
            }
            else if (result == d3d::mgpu::SsaoResult::failed)
            {
                m_trySecondary = false;
                m_historyFrame = 0;
            }
        }
        if (m_trySecondary && (m_forcedBusy || (!fresh && m_pendingFrame != 0)))
        {
            ++m_busyFrames;
        }
        if (m_trySecondary && m_historyFrame != 0)
        {
            m_captureActive = m_forcedBusy && m_captureRequested;
            if (reuseResult(filtered, fresh))
            {
                if (m_captureActive)
                {
                    m_captureDisplayFrame = m_frame;
                    m_captureSubmitted = true;
                }
                m_captureActive = false;
                upsample(filtered, m_historyDepth, output);
                return;
            }
        }
        m_historyFrame = 0;
        // Startup and actual failures use a primary result; a slow GPU 1 never stalls this path.
        if (!m_preparedNormals)
        {
            reconstructNormals(m_sourceNormals);
        }
        m_captureActive = !m_trySecondary && m_captureRequested;
        if (m_captureActive)
        {
            captureDepth(m_fullDepth, "source_full_depth");
        }
        drawRaw(raw, m_sourceNormals, m_depth);
        drawBlur(horizontal, raw, true);
        drawBlur(filtered, horizontal, false);
        if (m_captureActive)
        {
            m_captureDisplayFrame = m_frame;
            m_captureSubmitted = true;
        }
        m_captureActive = false;
        ++m_primaryFrames;
        updateDeviceStatus(false);
        upsample(filtered, m_depth, output);
    }

    bool AmbientOcclusion::reuseResult(BaseTexture* texture, bool fresh)
    {
        NAU_GPU_TIMER("ssao_reuse");
        if (!d3d::stretch_rect(m_history, texture))
        {
            m_historyFrame = 0;
            return false;
        }
        if (fresh)
        {
            ++m_secondaryFrames;
        }
        else
        {
            ++m_reusedFrames;
        }
        m_maxHistoryAge = eastl::max(m_maxHistoryAge, m_frame - m_historyFrame);
        updateDeviceStatus(true, !fresh);
        if (m_captureActive)
        {
            capture(m_history, "history", 1);
            capture(texture, "filtered", 1);
            capture(m_sourceNormals, "normals", 4);
            captureDepth();
            captureDepth(m_historyDepth, "history_depth");
            NAU_LOG_INFO("SSAO: reused capture frame={}, source={}", m_frame, m_historyFrame);
        }
        return true;
    }

    void AmbientOcclusion::draw(BaseTexture* target, MaterialAssetView& material)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(target, 0);
        TextureInfo info;
        target->getinfo(info);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, m_normals);
        d3d::settex(1, m_depth);
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

    void AmbientOcclusion::reconstructNormals(BaseTexture* target)
    {
        NAU_GPU_TIMER("ssao_reconstruct_normals");
        d3d::settex(1, m_depth);
        drawPipeline(target, "reconstructNormals");
    }

    void AmbientOcclusion::drawPipeline(BaseTexture* target, const char* pipeline)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(target, 0);
        TextureInfo size;
        target->getinfo(size);
        d3d::setview(0, 0, size.w, size.h, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        m_blurMaterial->bindPipeline(pipeline);
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 0; slot < 4; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::upsample(BaseTexture* source, BaseTexture* sourceDepth, BaseTexture* target)
    {
        NAU_GPU_TIMER("ssao_upsample");
        m_blurMaterial->setProperty("upsample", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, 1.0f));
        d3d::settex(1, sourceDepth);
        d3d::settex(2, source);
        d3d::settex(3, m_fullDepth);
        drawPipeline(target, "upsample");
        if (m_captureDisplayFrame == m_frame)
        {
            m_captureActive = true;
            capture(target, "resolved", 1);
            captureDepth(m_fullDepth, "resolve_depth");
            captureDepth(sourceDepth, "resolve_low_depth");
            m_captureActive = false;
        }
    }

    void AmbientOcclusion::drawRaw(BaseTexture* target, BaseTexture* normals, BaseTexture* depth)
    {
        NAU_GPU_TIMER("ssao_raw");
        m_normals = normals;
        m_depth = depth;
        m_rawMaterial->setProperty("default", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, m_mode != 0 ? 1.0f : 0.0f));
        draw(target, *m_rawMaterial);
        capture(target, "raw", 1);
        capture(m_sourceNormals, "normals", 4);
        captureDepth();
    }

    void AmbientOcclusion::drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal)
    {
        NAU_GPU_TIMER(horizontal ? "ssao_blur_h" : "ssao_blur_v");
        // A zero step samples only the center texel when blur is disabled.
        const float x = m_blur && horizontal ? 1.0f : 0.0f;
        const float y = m_blur && !horizontal ? 1.0f : 0.0f;
        m_blurMaterial->setProperty("default", "aoFilter", math::Vector4(x, y, m_blurDepthThreshold, m_mode != 0 ? 1.0f : 0.0f));
        d3d::settex(2, source);
        draw(target, *m_blurMaterial);
        if (!horizontal)
        {
            capture(target, "filtered", 1);
        }
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_mode != 0 ? 1.0f : 0.0f, m_mode == 2 ? 1.0f : 0.0f, 0.0f, 0.0f);
    }

    bool AmbientOcclusion::isDebugView() const
    {
        return m_mode == 2;
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

    void AmbientOcclusion::capture(BaseTexture* texture, const char* name, uint32_t components) const
    {
        if (!m_captureActive)
        {
            return;
        }
        void* pixels = nullptr;
        int stride = 0;
        if (!texture->lockimg(&pixels, stride, 0, TEXLOCK_READ) || !pixels)
        {
            NAU_LOG_ERROR("SSAO: could not read capture {}", name);
            return;
        }
        TextureInfo info;
        texture->getinfo(info);
        const eastl::string path = m_capturePrefix + "-" + name + ".f32";
        FILE* file = nullptr;
        const size_t rowBytes = static_cast<size_t>(info.w) * components * sizeof(float);
        const bool normalizedByte = (info.cflg & TEXFMT_MASK) == TEXFMT_R8;
        const bool packedNormals = (info.cflg & TEXFMT_MASK) == TEXFMT_G16R16F;
        bool succeeded = fopen_s(&file, path.c_str(), "wb") == 0 && file;
        if (file)
        {
            for (int row = 0; row < info.h; ++row)
            {
                if (normalizedByte)
                {
                    const uint8_t* values = static_cast<const uint8_t*>(pixels) + row * stride;
                    for (int column = 0; column < info.w; ++column)
                    {
                        const float value = values[column] / 255.0f;
                        succeeded &= fwrite(&value, sizeof(value), 1, file) == 1;
                    }
                }
                else if (packedNormals)
                {
                    const uint16_t* values = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pixels) + row * stride);
                    for (int column = 0; column < info.w; ++column)
                    {
                        const float expanded[4] = {decodeHalf(values[column * 2]), decodeHalf(values[column * 2 + 1]), 0.0f, 1.0f};
                        succeeded &= fwrite(expanded, sizeof(float), components, file) == components;
                    }
                }
                else
                {
                    succeeded &= fwrite(static_cast<const char*>(pixels) + row * stride, 1, rowBytes, file) == rowBytes;
                }
            }
            succeeded &= fclose(file) == 0;
        }
        texture->unlockimg();
        if (succeeded)
        {
            NAU_LOG_INFO("SSAO: capture {}, {}x{}, components={}", path.c_str(), info.w, info.h, components);
        }
        else
        {
            NAU_LOG_ERROR("SSAO: could not write capture {}", path.c_str());
        }
    }

    void AmbientOcclusion::captureDepth(BaseTexture* source, const char* name) const
    {
        if (!m_captureActive)
        {
            return;
        }
        TextureInfo info;
        source = source ? source : m_depth;
        source->getinfo(info);
        BaseTexture* copy = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_depth_capture");
        if (!copy)
        {
            return;
        }
        d3d::set_render_target(copy, 0);
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::setview(0, 0, info.w, info.h, 0.0f, 1.0f);
        d3d::settex(1, source);
        m_blurMaterial->bindPipeline("copyDepth");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(1, nullptr);
        d3d::set_render_target();
        capture(copy, name, 1);
        copy->destroy();
    }

    void AmbientOcclusion::capturePresentation(BaseTexture* backBuffer) const
    {
        if (m_captureDisplayFrame == 0 || m_frame != m_captureDisplayFrame)
        {
            return;
        }
        TextureInfo info;
        backBuffer->getinfo(info);
        BaseTexture* copy = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R8G8B8A8 | TEXCF_RTARGET, 1, u8"ssao_capture");
        if (!copy)
        {
            return;
        }
        d3d::stretch_rect(backBuffer, copy);
        void* pixels = nullptr;
        int stride = 0;
        if (copy->lockimg(&pixels, stride, 0, TEXLOCK_READ) && pixels)
        {
            const eastl::string path = m_capturePrefix + "-scene.ppm";
            FILE* file = nullptr;
            if (fopen_s(&file, path.c_str(), "wb") == 0 && file)
            {
                bool succeeded = fprintf(file, "P6\n%d %d\n255\n", info.w, info.h) > 0;
                for (int y = 0; y < info.h; ++y)
                {
                    const uint8_t* row = static_cast<const uint8_t*>(pixels) + y * stride;
                    for (int x = 0; x < info.w; ++x)
                    {
                        succeeded &= fwrite(row + x * 4, 1, 3, file) == 3;
                    }
                }
                succeeded &= fclose(file) == 0;
                if (succeeded)
                {
                    NAU_LOG_INFO("SSAO: captured presentation {}", path.c_str());
                }
            }
            copy->unlockimg();
        }
        copy->destroy();
    }
}  // namespace nau::render
