// ambient_occlusion.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include "graphics_assets/material_asset.h"
#include "nau/3d/multi_gpu_ssao.h"
#include "nau/render/render_window.h"

namespace nau::render
{
    class AmbientOcclusion final
    {
    public:
        AmbientOcclusion() = default;
        AmbientOcclusion(const AmbientOcclusion&) = delete;
        AmbientOcclusion(AmbientOcclusion&&) = delete;
        AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;
        AmbientOcclusion& operator=(AmbientOcclusion&&) = delete;
        ~AmbientOcclusion();

        static bool isRequested();
        static AmbientOcclusionMode getInitialMode();
        async::Task<> loadAssets();
        void prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode, BaseTexture* normals, BaseTexture* depth);
        void render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, BaseTexture* source = nullptr);
        void drawRaw(BaseTexture* target);
        void drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal);
        void resetSecondary();
        void downsampleInputs();
        void drawSecondaryFilter(BaseTexture* target, BaseTexture* source, const char* pipeline);
        bool prepareSecondary(BaseTexture* depth);

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_rawShader;
        d3d::mgpu::SsaoProducer* m_secondary = nullptr;
        d3d::mgpu::SsaoConstants m_constants;
        d3d::mgpu::PrimarySyncPoint m_secondaryReady;
        BaseTexture* m_normalsSnapshot = nullptr;
        BaseTexture* m_depthSnapshot = nullptr;
        BaseTexture* m_receiving = nullptr;
        BaseTexture* m_filtered = nullptr;
        uint32_t m_secondaryWidth = 0;
        uint32_t m_secondaryHeight = 0;
        uint32_t m_primaryWidth = 0;
        uint32_t m_primaryHeight = 0;
        bool m_useSecondary = false;
        bool m_secondaryAttempted = false;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_depth = nullptr;
        math::Vector4 m_parameters = math::Vector4(0.5f, 0.2f, 1.0f, 0.05f);
        float m_blurDepthThreshold = 0.2f;
        AmbientOcclusionMode m_mode = AmbientOcclusionMode::lighting;
        bool m_blur = true;
    };
}  // namespace nau::render
