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
        m_reprojection = readSetting<bool>("render/ssao/reprojection", true);
        m_reprojectionDepthThreshold = readFiniteSetting("render/ssao/reprojectionDepthThreshold", 0.1f, 0.0001f, 10.0f);
        m_useSecondary = readSetting<bool>("render/ssao/useSecondary", false);
        if (m_useSecondary && !m_blurMaterial->getPipelineNames().contains("packNormals"))
        {
            m_useSecondary = false;
            NAU_LOG_WARNING("SSAO: normal packing pipeline unavailable, using GPU 0");
        }
        if (m_useSecondary && m_reprojection && !m_blurMaterial->getPipelineNames().contains("reproject"))
        {
            m_useSecondary = false;
            NAU_LOG_WARNING("SSAO: reprojection pipeline unavailable, using GPU 0");
        }
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
        if (m_useSecondary && !d3d::is_multi_gpu_available())
        {
            NAU_LOG_WARNING("SSAO: using GPU 0: {}", d3d::get_multi_gpu_fallback_reason());
        }
        NAU_LOG_INFO("SSAO: full resolution, G-buffer normals");
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
        for (BaseTexture** texture : {&m_normalsSnapshot, &m_depthSnapshot, &m_receiving, &m_history, &m_historyDepth})
        {
            if (*texture)
            {
                (*texture)->destroy();
                *texture = nullptr;
            }
        }
        m_historyValid = false;
        m_discardPending = false;
    }

    void AmbientOcclusion::beginFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode)
    {
        if (m_mode != mode)
        {
            resetSecondary();
            m_secondaryAttempted = false;
            m_mode = mode;
            m_historyValid = false;
            m_discardPending = true;
            NAU_LOG_INFO("SSAO: mode={}", static_cast<uint32_t>(m_mode));
        }
        m_rawMaterial->setProperty("default", "aoProjection", projection);
        m_rawMaterial->setProperty("default", "aoParameters", m_parameters);
        const math::Matrix4 inverseProjection = inverse(projection);
        m_viewProjection = projection * view;
        m_inverseProjection = inverseProjection;
        static_assert(sizeof(projection) == sizeof(m_constants.projection));
        std::memcpy(m_constants.projection, &projection, sizeof(projection));
        std::memcpy(m_constants.inverseProjection, &inverseProjection, sizeof(inverseProjection));
        std::memcpy(m_constants.view, &view, sizeof(view));
        std::memcpy(m_constants.parameters, &m_parameters, sizeof(m_parameters));
        m_constants.filter[2] = m_blurDepthThreshold;
        for (MaterialAssetView* material : {m_rawMaterial.get(), m_blurMaterial.get()})
        {
            material->setProperty("default", "aoInverseProjection", inverseProjection);
            material->setProperty("default", "aoView", view);
        }
    }

    void AmbientOcclusion::prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode, BaseTexture* normals, BaseTexture* depth)
    {
        beginFrame(view, projection, mode);
        m_normals = normals;
        m_depth = depth;
        if (m_mode == AmbientOcclusionMode::off || !m_useSecondary || !prepareSecondary(depth) || d3d::mgpu::isSsaoBusy(m_secondary))
        {
            return;
        }
        packNormals(normals);
        if (!m_depthSnapshot->update(depth))
        {
            d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
            m_historyValid = false;
            return;
        }
        const d3d::mgpu::SsaoResult submitted = d3d::mgpu::submitSsao(m_secondary, m_constants, m_normalsSnapshot, m_depthSnapshot);
        if (submitted == d3d::mgpu::SsaoResult::failed)
        {
            m_historyValid = false;
        }
        if (submitted == d3d::mgpu::SsaoResult::submitted)
        {
            m_pendingViewProjection = m_viewProjection;
            m_pendingInverseProjection = m_inverseProjection;
            m_discardPending = false;
        }
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
        if (!m_useSecondary || !receiveSecondary(raw))
        {
            drawRaw(raw);
        }
        // Filter after reprojection, against this frame's depth and normals.
        drawBlur(horizontal, raw, true);
        drawBlur(output, horizontal, false);
    }

    bool AmbientOcclusion::prepareSecondary(BaseTexture* depth)
    {
        if (!d3d::is_multi_gpu_available() || !m_vertexShader || !m_rawShader)
        {
            m_historyValid = false;
            return false;
        }
        TextureInfo info;
        depth->getinfo(info);
        if (m_secondaryWidth != info.w || m_secondaryHeight != info.h)
        {
            resetSecondary();
            m_secondaryWidth = info.w;
            m_secondaryHeight = info.h;
            m_secondaryAttempted = false;
        }
        if (!m_secondaryAttempted)
        {
            m_secondaryAttempted = true;
            const Shader* vertex = m_vertexShader->getShader();
            const Shader* raw = m_rawShader->getShader();
            d3d::mgpu::SsaoProducerDesc desc;
            desc.width = info.w;
            desc.height = info.h;
            desc.vertexBytecode = vertex->bytecode.data();
            desc.vertexBytecodeSize = vertex->bytecode.size();
            desc.rawBytecode = raw->bytecode.data();
            desc.rawBytecodeSize = raw->bytecode.size();
            m_secondary = d3d::mgpu::createSsaoProducer(desc);
            if (!m_secondary)
            {
                return false;
            }
            // SSAO reads only the sphere-map .xy encoding; pack it without reconstruction or resampling.
            m_normalsSnapshot = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_G32R32F | TEXCF_RTARGET, 1, u8"ssao_normals_snapshot");
            m_depthSnapshot = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_DEPTH32 | TEXCF_RTARGET, 1, u8"ssao_depth_snapshot");
            m_receiving = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_receiving");
            m_history = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_R8 | TEXCF_RTARGET, 1, u8"ssao_history");
            if (m_reprojection)
            {
                m_historyDepth = d3d::create_tex(nullptr, info.w, info.h, TEXFMT_DEPTH32 | TEXCF_RTARGET, 1, u8"ssao_history_depth");
            }
            if (!m_normalsSnapshot || !m_depthSnapshot || !m_receiving || !m_history || (m_reprojection && !m_historyDepth))
            {
                d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_OUTOFMEMORY));
                resetSecondary();
                return false;
            }
            NAU_LOG_INFO("SSAO: GPU 1 ready, {}x{}, three shared slots, deferred copies", info.w, info.h);
            NAU_LOG_INFO("SSAO: camera reprojection={}", m_reprojection);
        }
        if (!m_secondary)
        {
            return false;
        }
        return true;
    }

    bool AmbientOcclusion::receiveSecondary(BaseTexture* target)
    {
        if (!m_secondary || !d3d::is_multi_gpu_available())
        {
            return false;
        }
        const d3d::mgpu::SsaoResult received = d3d::mgpu::tryReceiveSsao(m_secondary, m_receiving);
        if (received == d3d::mgpu::SsaoResult::failed)
        {
            m_historyValid = false;
            return false;
        }
        if (received == d3d::mgpu::SsaoResult::submitted && !m_discardPending)
        {
            // The snapshot still belongs to this result. Preserve it before the next submit overwrites it.
            if (m_reprojection && !m_historyDepth->update(m_depthSnapshot))
            {
                d3d::report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
                m_historyValid = false;
                return false;
            }
            eastl::swap(m_history, m_receiving);
            m_historyViewProjection = m_pendingViewProjection;
            m_historyInverseProjection = m_pendingInverseProjection;
            if (!m_historyValid)
            {
                NAU_LOG_INFO("SSAO: displaying completed GPU 1 result");
            }
            m_historyValid = true;
        }
        if (!m_historyValid)
        {
            return false;
        }
        // Reproject the original result each frame; never feed a reprojected map back into history.
        if (m_reprojection)
        {
            reprojectHistory(target, m_depth);
            return true;
        }
        m_blurMaterial->setProperty("default", "aoFilter", math::Vector4(0.0f, 0.0f, m_blurDepthThreshold, 1.0f));
        draw(target, *m_blurMaterial, m_history);
        return true;
    }

    void AmbientOcclusion::reprojectHistory(BaseTexture* target, BaseTexture* depth)
    {
        const math::Matrix4 currentToHistory = m_historyViewProjection * inverse(m_viewProjection);
        m_blurMaterial->setProperty("reproject", "aoCurrentToHistory", currentToHistory);
        m_blurMaterial->setProperty("reproject", "aoHistoryInverseProjection", m_historyInverseProjection);
        m_blurMaterial->setProperty("reproject", "aoReprojectionParameters", math::Vector4(m_reprojectionDepthThreshold, 0.0f, 0.0f, 0.0f));
        for (BaseTexture* source : {depth, m_history, m_historyDepth})
        {
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(target, 0);
        d3d::setview(0, 0, m_secondaryWidth, m_secondaryHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(1, depth);
        d3d::settex(2, m_history);
        d3d::settex(3, m_historyDepth);
        m_blurMaterial->bindPipeline("reproject");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        for (int slot = 1; slot <= 3; ++slot)
        {
            d3d::settex(slot, nullptr);
        }
        d3d::set_render_target();
    }

    void AmbientOcclusion::packNormals(BaseTexture* source)
    {
        d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_render_target(m_normalsSnapshot, 0);
        d3d::setview(0, 0, m_secondaryWidth, m_secondaryHeight, 0.0f, 1.0f);
        d3d::set_srgb_backbuffer_write(false);
        d3d::settex(0, source);
        m_blurMaterial->bindPipeline("packNormals");
        d3d::setvsrc(0, nullptr, 0);
        d3d::setind(nullptr);
        d3d::draw(PRIM_TRILIST, 0, 1);
        d3d::settex(0, nullptr);
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
