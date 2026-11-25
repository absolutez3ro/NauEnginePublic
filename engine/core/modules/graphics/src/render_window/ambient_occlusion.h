// ambient_occlusion.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include "graphics_assets/material_asset.h"
#include "nau/render/render_window.h"

namespace nau::render
{
    /** @brief Single-device SSAO and its half-resolution working textures. */
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
        void render(const math::Matrix4& projection, BaseTexture& depth, AmbientOcclusionMode mode);
        void drawDebug(BaseTexture& target);
        bool isDebugView() const;
        void invalidate();
        void configureResolveSampler(MaterialAssetView& material) const;
        void bindResolveInputs(BaseTexture& fallbackAo, BaseTexture& fallbackDepth) const;

    private:
        bool prepareTargets(BaseTexture& depth);
        void releaseTargets();
        void draw(BaseTexture& target, MaterialAssetView& material, const char* pipeline, BaseTexture& depth, BaseTexture* source = nullptr);

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_postMaterial;
        d3d::SamplerHandle m_linearClamp = {};
        BaseTexture* m_fullDepth = nullptr;
        BaseTexture* m_halfDepth = nullptr;
        BaseTexture* m_result = nullptr;
        BaseTexture* m_blurredResult = nullptr;
        math::Matrix4 m_projection;
        math::Matrix4 m_inverseProjection;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        AmbientOcclusionMode m_mode = AmbientOcclusionMode::off;
        bool m_valid = false;
    };
}  // namespace nau::render
