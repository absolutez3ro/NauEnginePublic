// ambient_occlusion.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nau/app/global_properties.h"
#include "nau/assets/asset_manager.h"
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

    void AmbientOcclusionInput::setTarget(void* windowHandle)
    {
        m_windowHandle.store(windowHandle);
        m_switchRequests.store(0);
        m_deviceSwitchRequests.store(0);
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
        m_vertexShader = co_await vertex->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_rawShader = co_await rawShader->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        m_blurShader = co_await blurShader->getAssetView(rtti::getTypeInfo<ShaderAssetView>());
        const float radius = readFiniteSetting("sample/ssao/radius", 0.5f, 0.0f, 10.0f);
        const float fadeStart = readFiniteSetting("sample/ssao/fadeStart", 0.2f, 0.0f, 10.0f);
        const float fadeEnd = readFiniteSetting("sample/ssao/fadeEnd", 1.0f, fadeStart + 0.001f, 20.0f);
        const float epsilon = readFiniteSetting("sample/ssao/surfaceEpsilon", 0.05f, 0.0001f, fadeEnd);
        m_parameters = math::Vector4(radius, fadeStart, fadeEnd, epsilon);
        m_blurDepthThreshold = readFiniteSetting("sample/ssao/blurDepthThreshold", 0.2f, 0.0f, 10.0f);
        m_mode = eastl::min(readSetting<uint32_t>("sample/ssao/mode", 1), 2u);
        m_blur = readSetting<bool>("sample/ssao/blur", true);
        m_captureAtFrame = readSetting<uint32_t>("sample/ssao/captureAtFrame", 0);
        m_autoSwitchFrames = readSetting<uint32_t>("sample/ssao/autoSwitchFrames", 0);
        m_capturePrefix = readSetting<eastl::string>("sample/ssao/capturePrefix", "ssao");
        m_requestedSecondary = readSetting<bool>("sample/ssao/useSecondary", true);
        m_validateSecondary = readSetting<bool>("sample/ssao/validateSecondary", false);
        m_warpForTesting = readSetting<bool>("dx12/secondaryWarpForTesting", false);
        m_autoDeviceSwitchFrames = readSetting<uint32_t>("sample/ssao/autoDeviceSwitchFrames", 0);
        m_failAtFrame = readSetting<uint32_t>("sample/ssao/failAtFrame", 0);
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        if (m_initialized)
        {
            getServiceProvider().get<AmbientOcclusionInput>().setTarget(nullptr);
        }
        d3d::mgpu::destroySsaoProducer(m_secondary);
        NAU_LOG_INFO("SSAO: shutdown, frames={}, secondary={}, primary={}, busy={}", m_frame, m_secondaryFrames, m_primaryFrames, m_busyFrames);
    }

    void AmbientOcclusion::beginFrame(const math::Matrix4& view, const math::Matrix4& projection, void* windowHandle)
    {
        AmbientOcclusionInput& input = getServiceProvider().get<AmbientOcclusionInput>();
        m_windowHandle = windowHandle;
        if (!m_initialized)
        {
            input.setTarget(windowHandle);
            m_initialized = true;
            NAU_LOG_INFO("SSAO: 14 samples, bilateral blur={}, F7: off / lit / AO, F8: GPU 0 / GPU 1", m_blur);
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
        }
        if (m_failAtFrame != 0 && m_frame == m_failAtFrame)
        {
            NAU_LOG_WARNING("SSAO: injecting secondary failure at frame {}", m_frame);
            d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
        }
        uint32_t switches = input.consumeSwitches();
        if (m_autoSwitchFrames != 0 && m_frame % m_autoSwitchFrames == 0)
        {
            ++switches;
        }
        m_mode = (m_mode + switches) % 3;
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
    }

    void AmbientOcclusion::prepareSecondary(BaseTexture* target)
    {
        TextureInfo info;
        target->getinfo(info);
        if (m_secondaryWidth != info.w || m_secondaryHeight != info.h)
        {
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
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = info.w;
            desc.height = info.h;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            desc.blurBytecode = blur->bytecode.data();
            desc.blurBytecodeSize = blur->bytecode.size();
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            NAU_LOG_INFO("SSAO: secondary pass ready={}, size={}x{}", m_secondary != nullptr, info.w, info.h);
        }
    }

    void AmbientOcclusion::updateDeviceStatus(bool secondary)
    {
        eastl::string status = secondary ? (m_warpForTesting ? "GPU 1 WARP -> GPU 0 (TEST)" : "GPU 1 -> GPU 0") : "GPU 0";
        if (m_requestedSecondary && !d3d::is_multi_gpu_available())
        {
            status += " | ";
            status += d3d::get_multi_gpu_fallback_reason();
        }
        if (status != m_deviceStatus)
        {
            m_deviceStatus = status;
            NAU_LOG_INFO("SSAO: device={}, frame={}", status.c_str(), m_frame);
            const eastl::string title = "SSAO | " + status + " | F7: view | F8: GPU";
            SetWindowTextA(static_cast<HWND>(m_windowHandle), title.c_str());
        }
    }

    void AmbientOcclusion::renderFrame(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* filtered, BaseTexture* normals, BaseTexture* depth)
    {
        m_normals = normals;
        m_depth = depth;
        prepareSecondary(filtered);
        const bool capturing = m_captureAtFrame != 0 && m_frame == m_captureAtFrame;
        if (m_requestedSecondary && m_mode != 0 && m_secondary && d3d::is_multi_gpu_available())
        {
            if (capturing && m_validateSecondary)
            {
                // Reference and secondary results use identical inputs and matrices from this frame.
                drawRaw(raw, normals, depth);
                drawBlur(horizontal, raw, true);
                drawBlur(filtered, horizontal, false);
                capture(filtered, "reference", 1);
            }
            const d3d::mgpu::SsaoResult result = d3d::mgpu::renderSsao(m_secondary, m_constants, m_blur, normals, depth, filtered,
                                                                       capturing ? raw : nullptr);
            if (result == d3d::mgpu::SsaoResult::submitted)
            {
                ++m_secondaryFrames;
                updateDeviceStatus(true);
                if (capturing)
                {
                    capture(raw, "raw", 1);
                    capture(filtered, "filtered", 1);
                    capture(normals, "normals", 4);
                    captureDepth();
                    NAU_LOG_INFO("SSAO: secondary capture frame={}", m_frame);
                }
                return;
            }
            if (result == d3d::mgpu::SsaoResult::busy)
            {
                ++m_busyFrames;
            }
        }
        // Always compute this frame on fallback: AO from an older camera/depth is not valid here.
        drawRaw(raw, normals, depth);
        drawBlur(horizontal, raw, true);
        drawBlur(filtered, horizontal, false);
        ++m_primaryFrames;
        updateDeviceStatus(false);
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

    void AmbientOcclusion::drawRaw(BaseTexture* target, BaseTexture* normals, BaseTexture* depth)
    {
        m_normals = normals;
        m_depth = depth;
        m_rawMaterial->setProperty("default", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, m_mode != 0 ? 1.0f : 0.0f));
        draw(target, *m_rawMaterial);
        capture(target, "raw", 1);
        capture(normals, "normals", 4);
        captureDepth();
    }

    void AmbientOcclusion::drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal)
    {
        // A zero step reduces the filter to the center sample, useful for inspecting the unfiltered result.
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
        if (m_frame != m_captureAtFrame || m_captureAtFrame == 0)
        {
            return;
        }
        // Opt-in validation readback. Rendering normally stays entirely on the GPU.
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
        bool succeeded = fopen_s(&file, path.c_str(), "wb") == 0 && file;
        if (file)
        {
            for (int row = 0; row < info.h; ++row)
            {
                succeeded &= fwrite(static_cast<const char*>(pixels) + row * stride, 1, rowBytes, file) == rowBytes;
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

    void AmbientOcclusion::captureDepth() const
    {
        if (m_frame != m_captureAtFrame || m_captureAtFrame == 0)
        {
            return;
        }
        TextureInfo info;
        m_depth->getinfo(info);
        BaseTexture* copy = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R32F | TEXCF_RTARGET, 1, u8"ssao_depth_capture");
        if (!copy)
        {
            return;
        }
        d3d::set_render_target(copy, 0);
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::settex(1, m_depth);
        m_blurMaterial->bindPipeline("copyDepth");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(1, nullptr);
        d3d::set_render_target();
        capture(copy, "depth", 1);
        copy->destroy();
    }

    void AmbientOcclusion::capturePresentation(BaseTexture* backBuffer) const
    {
        if (m_frame != m_captureAtFrame || m_captureAtFrame == 0)
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
