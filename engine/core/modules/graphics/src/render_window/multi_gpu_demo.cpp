// multi_gpu_demo.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "multi_gpu_demo.h"

#include <cstdio>

#include "nau/app/global_properties.h"
#include "nau/assets/asset_manager.h"
#include "nau/diag/logging.h"
#include "nau/math/dag_color.h"
#include "nau/platform/windows/windows_headers.h"
#include "nau/service/service_provider.h"

namespace nau::render
{
    void MultiGpuDemoInput::setTarget(void* windowHandle)
    {
        m_windowHandle.store(windowHandle);
        m_switchRequests.store(0);
    }

    bool MultiGpuDemoInput::consumeSwitch()
    {
        return (m_switchRequests.exchange(0) & 1u) != 0;
    }

    bool MultiGpuDemoInput::handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM)
    {
        if (hwnd == m_windowHandle.load() && message == WM_KEYUP && wParam == VK_F6)
        {
            m_switchRequests.fetch_add(1);
            return true;
        }
        return false;
    }

    namespace
    {
        template <typename T>
        T readSetting(const char* name, T fallback)
        {
            GlobalProperties& properties = getServiceProvider().get<GlobalProperties>();
            return properties.getValue<T>(name).value_or(fallback);
        }
    }  // namespace

    bool MultiGpuDemo::isRequested()
    {
        return readSetting<bool>("sample/mgpuDemo/enabled", false);
    }

    async::Task<> MultiGpuDemo::loadAssets()
    {
        MaterialAssetRef material{"file:/res/materials/mgpu_demo.nmat_json"};
        m_material = co_await material.getAssetViewTyped<MaterialAssetView>();
        IAssetManager& assets = getServiceProvider().get<IAssetManager>();
        IAssetDescriptor::Ptr vertex = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[mgpu_demo.vs.vsmain]"});
        IAssetDescriptor::Ptr pixel = assets.openAsset(AssetPath{"file:/res/shaders/cache/shader_cache.nsbc+[mgpu_demo.ps.psmain]"});
        m_vertexShader = co_await vertex->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_pixelShader = co_await pixel->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
    }

    bool MultiGpuDemo::initialize(void* windowHandle)
    {
        m_initialized = true;
        getServiceProvider().get<MultiGpuDemoInput>().setTarget(windowHandle);
        m_start = std::chrono::steady_clock::now();
        m_lastTransfer = m_start;
        m_requestedSecondary = readSetting<bool>("sample/mgpuDemo/useSecondary", true);
        m_warpForTesting = readSetting<bool>("dx12/secondaryWarpForTesting", false);
        m_autoSwitchFrames = readSetting<uint32_t>("sample/mgpuDemo/autoSwitchFrames", 0);
        m_failAtFrame = readSetting<uint32_t>("sample/mgpuDemo/failAtFrame", 0);
        m_captureAtFrame = readSetting<uint32_t>("sample/mgpuDemo/captureAtFrame", 0);
        m_capturePath = readSetting<eastl::string>("sample/mgpuDemo/capturePath", "mgpu-demo.ppm");
        m_destination = d3d::create_tex(nullptr, WIDTH, HEIGHT, TEXFMT_R8G8B8A8 | TEXCF_RTARGET, 1, u8"mgpu_demo_destination");
        if (!m_destination || !m_material || !m_vertexShader || !m_pixelShader)
        {
            NAU_LOG_ERROR("MG-12: demo assets or primary destination unavailable");
            return false;
        }
        if (d3d::is_multi_gpu_available())
        {
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* pixel = m_pixelShader->getShader();
            d3d::mgpu::FullscreenProducerDesc desc;
            desc.width = WIDTH;
            desc.height = HEIGHT;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.pixelBytecode = pixel->bytecode.data();
            desc.pixelBytecodeSize = pixel->bytecode.size();
            m_producer = d3d::mgpu::createFullscreenProducer(desc);
        }
        NAU_LOG_INFO("MG-12: demo ready, {}x{}, F6 switches GPU 0 / secondary; secondary available={}", WIDTH, HEIGHT,
                     m_producer != nullptr);
        return true;
    }

    MultiGpuDemo::~MultiGpuDemo()
    {
        getServiceProvider().get<MultiGpuDemoInput>().setTarget(nullptr);
        // Producer destruction flushes the engine and channel before releasing the native pass resources.
        d3d::mgpu::destroyFullscreenProducer(m_producer);
        if (m_destination)
        {
            m_destination->destroy();
        }
        NAU_LOG_INFO("MG-12: shutdown, frames={}, transferred={}, primary={}", m_frame, m_transferredFrames, m_primaryFrames);
    }

    void MultiGpuDemo::drawOnPrimary(const math::Vector4& constants)
    {
        d3d::set_render_target();
        d3d::set_render_target(m_destination, 0);
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::setview(0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
        m_material->setProperty("default", "g_passConstants", constants);
        m_material->bind();
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        ++m_primaryFrames;
    }

    void MultiGpuDemo::updateStatus(const char* mode, void* windowHandle)
    {
        eastl::string status = mode;
        if (m_requestedSecondary && !d3d::is_multi_gpu_available())
        {
            status += " | ";
            status += d3d::get_multi_gpu_fallback_reason();
        }
        if (m_status != status)
        {
            m_status = status;
            NAU_LOG_INFO("MG-12: mode={}, frame={}", status.c_str(), m_frame);
            const eastl::string title = "MG-12 | " + status + " | F6: switch 1/2 GPU";
            SetWindowTextA(static_cast<HWND>(windowHandle), title.c_str());
        }
    }

    void MultiGpuDemo::render(BaseTexture* backBuffer, void* windowHandle)
    {
        if (!m_initialized && !initialize(windowHandle))
        {
            return;
        }
        if (!m_destination || !m_material)
        {
            return;
        }
        ++m_frame;
        const bool switchPressed = getServiceProvider().get<MultiGpuDemoInput>().consumeSwitch();
        const bool automaticSwitch = m_autoSwitchFrames != 0 && m_frame % m_autoSwitchFrames == 0;
        if (switchPressed || automaticSwitch)
        {
            m_requestedSecondary = !m_requestedSecondary;
            m_hasSecondaryFrame = false;
            m_lastTransfer = std::chrono::steady_clock::now();
        }

        if (m_failAtFrame != 0 && m_frame >= m_failAtFrame && !m_failureInjected)
        {
            m_failureInjected = true;
            NAU_LOG_WARNING("MG-12: injecting transfer failure for acceptance test at frame {}", m_frame);
            d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
        }

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - m_start).count();
        const uint32_t phase = static_cast<uint32_t>(seconds * 30.0) % 256;
        const math::Vector4 constants(static_cast<float>(phase), static_cast<float>(WIDTH), static_cast<float>(HEIGHT), 0.0f);
        bool usingSecondary = false;
        if (m_requestedSecondary && m_producer && d3d::is_multi_gpu_available())
        {
            const float nativeConstants[4] = {static_cast<float>(phase), static_cast<float>(WIDTH), static_cast<float>(HEIGHT), 0.0f};
            const d3d::mgpu::FullscreenResult result = d3d::mgpu::renderFullscreen(m_producer, nativeConstants, m_destination);
            if (result == d3d::mgpu::FullscreenResult::submitted)
            {
                m_visiblePhase = phase;
                m_lastTransfer = now;
                m_hasSecondaryFrame = true;
                ++m_transferredFrames;
            }
            // Queue pressure is a dropped frame, not a device failure. A stalled channel eventually triggers MG-11.
            if (result == d3d::mgpu::FullscreenResult::busy && now - m_lastTransfer > std::chrono::seconds(5))
            {
                NAU_LOG_WARNING("MG-12: no completed transfer submission for five seconds, falling back");
                d3d::report_secondary_gpu_failure(static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
            }
            usingSecondary = m_hasSecondaryFrame && d3d::is_multi_gpu_available();
        }
        d3d::set_srgb_backbuffer_write(false);
        if (!usingSecondary)
        {
            drawOnPrimary(constants);
            m_visiblePhase = phase;
        }
        d3d::stretch_rect(m_destination, backBuffer);
        updateStatus(usingSecondary ? (m_warpForTesting ? "GPU 1 WARP -> GPU 0 (TEST)" : "GPU 1 -> GPU 0") : "GPU 0", windowHandle);
        if (m_frame == m_captureAtFrame)
        {
            captureFrame();
        }
        if (m_frame % 120 == 0)
        {
            NAU_LOG_INFO("MG-12: frame={}, transferred={}, primary={}, phase={}", m_frame, m_transferredFrames, m_primaryFrames,
                         m_visiblePhase);
        }
    }

    void MultiGpuDemo::captureFrame()
    {
        // Optional acceptance-test readback only; normal rendering never copies pixels to the CPU.
        void* pixels = nullptr;
        int stride = 0;
        if (!m_destination->lockimg(&pixels, stride, 0, TEXLOCK_READ) || !pixels)
        {
            NAU_LOG_ERROR("MG-12: capture readback failed");
            return;
        }
        FILE* file = nullptr;
        fopen_s(&file, m_capturePath.c_str(), "wb");
        if (file)
        {
            fprintf(file, "P6\n# MG-12 frame=%llu phase=%u\n%u %u\n255\n", static_cast<unsigned long long>(m_frame), m_visiblePhase, WIDTH,
                    HEIGHT);
            for (uint32_t y = 0; y < HEIGHT; ++y)
            {
                const uint8_t* row = static_cast<const uint8_t*>(pixels) + y * stride;
                for (uint32_t x = 0; x < WIDTH; ++x)
                {
                    fwrite(row + x * 4, 1, 3, file);
                }
            }
            fclose(file);
            NAU_LOG_INFO("MG-12: captured GPU 0 destination at frame={}, phase={}, path={}", m_frame, m_visiblePhase,
                         m_capturePath.c_str());
        }
        else
        {
            NAU_LOG_ERROR("MG-12: cannot write capture {}", m_capturePath.c_str());
        }
        m_destination->unlockimg();
    }
}  // namespace nau::render
