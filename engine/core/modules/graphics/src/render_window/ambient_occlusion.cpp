// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion.h"

#include <EASTL/algorithm.h>

#include <cmath>

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

    void AmbientOcclusionInput::setTarget(void* windowHandle)
    {
        m_windowHandle.store(windowHandle);
        m_switchRequests.store(0);
    }

    uint32_t AmbientOcclusionInput::consumeSwitches()
    {
        return m_switchRequests.exchange(0);
    }

    bool AmbientOcclusionInput::handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM)
    {
        if (hwnd == m_windowHandle.load() && message == WM_KEYUP && wParam == VK_F7)
        {
            m_switchRequests.fetch_add(1);
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

        const float radius = readFiniteSetting("sample/ssao/radius", 0.5f, 0.0f, 10.0f);
        const float fadeStart = readFiniteSetting("sample/ssao/fadeStart", 0.2f, 0.0f, 10.0f);
        const float fadeEnd = readFiniteSetting("sample/ssao/fadeEnd", 1.0f, fadeStart + 0.001f, 20.0f);
        const float epsilon = readFiniteSetting("sample/ssao/surfaceEpsilon", 0.05f, 0.0001f, fadeEnd);
        m_parameters = math::Vector4(radius, fadeStart, fadeEnd, epsilon);
        m_blurDepthThreshold = readFiniteSetting("sample/ssao/blurDepthThreshold", 0.2f, 0.0f, 10.0f);
        m_mode = eastl::min(readSetting<uint32_t>("sample/ssao/mode", 1), 2u);
        m_blur = readSetting<bool>("sample/ssao/blur", true);
    }

    AmbientOcclusion::~AmbientOcclusion()
    {
        if (m_initialized)
        {
            getServiceProvider().get<AmbientOcclusionInput>().setTarget(nullptr);
        }
    }

    void AmbientOcclusion::beginFrame(const math::Matrix4& view, const math::Matrix4& projection, void* windowHandle)
    {
        AmbientOcclusionInput& input = getServiceProvider().get<AmbientOcclusionInput>();
        if (!m_initialized)
        {
            input.setTarget(windowHandle);
            m_initialized = true;
            NAU_LOG_INFO("SSAO: full resolution, G-buffer normals, F7: off / lit / AO");
        }
        const uint32_t switches = input.consumeSwitches();
        m_mode = (m_mode + switches % 3u) % 3u;
        if (switches != 0)
        {
            NAU_LOG_INFO("SSAO: mode={}", m_mode);
        }
        m_rawMaterial->setProperty("default", "aoProjection", projection);
        m_rawMaterial->setProperty("default", "aoParameters", m_parameters);
        const math::Matrix4 inverseProjection = inverse(projection);
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_blurMaterial.get()})
        {
            material->setProperty("default", "aoInverseProjection", inverseProjection);
            material->setProperty("default", "aoView", view);
        }
    }

    void AmbientOcclusion::render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth)
    {
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        if (m_mode == 0)
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
        drawRaw(raw);
        drawBlur(horizontal, raw, true);
        drawBlur(output, horizontal, false);
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
        return m_mode == 2;
    }

    math::Vector4 AmbientOcclusion::getResolveControl() const
    {
        return math::Vector4(m_mode != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    }
}  // namespace nau::render
