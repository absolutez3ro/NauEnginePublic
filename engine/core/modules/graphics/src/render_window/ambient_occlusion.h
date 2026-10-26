// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include "graphics_assets/material_asset.h"
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

        static bool isRequested();
        async::Task<> loadAssets();
        void beginFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode);
        void render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;
        AmbientOcclusionMode getMode() const;

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, BaseTexture* source = nullptr);
        void drawRaw(BaseTexture* target);
        void drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal);

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_depth = nullptr;
        math::Vector4 m_parameters = math::Vector4(0.5f, 0.2f, 1.0f, 0.05f);
        float m_blurDepthThreshold = 0.2f;
        AmbientOcclusionMode m_mode = AmbientOcclusionMode::Lighting;
        bool m_blur = true;
    };
}  // namespace nau::render
