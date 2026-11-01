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
        struct Settings
        {
            AmbientOcclusionMode mode = AmbientOcclusionMode::lighting;
            AmbientOcclusionExecution execution = AmbientOcclusionExecution::singleGpu;
            AmbientOcclusionVariant variant = AmbientOcclusionVariant::current;
        };

        AmbientOcclusion() = default;
        AmbientOcclusion(const AmbientOcclusion&) = delete;
        AmbientOcclusion(AmbientOcclusion&&) = delete;
        AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;
        AmbientOcclusion& operator=(AmbientOcclusion&&) = delete;
        ~AmbientOcclusion();

        static bool isRequested();
        static Settings getInitialSettings();
        async::Task<> loadAssets();
        void prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, const Settings& settings, BaseTexture* normals, BaseTexture* depth);
        void render(BaseTexture* output);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;
        const char* getExecutionLabel() const;
        const char* getVariantLabel() const;
        BaseTexture* getResolveTexture(BaseTexture* fullSize) const;
        void configureResolveSampler(MaterialAssetView& material) const;

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source = nullptr);
        void filter(BaseTexture* output);
        void resetSecondary();
        void downsampleInputs();
        bool prepareTargets(BaseTexture* depth);
        bool prepareSecondary();
        void logSettings();
        bool createRandomTexture();

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_rawShader;
        ShaderAssetView::Ptr m_bogdanovShader;
        Texture* m_randomTexture = nullptr;
        d3d::SamplerHandle m_linearClamp = {};
        d3d::SamplerHandle m_linearWrap = {};
        d3d::mgpu::SsaoProducer* m_secondary = nullptr;
        d3d::mgpu::SsaoConstants m_constants;
        d3d::mgpu::PrimarySyncPoint m_secondaryReady;
        BaseTexture* m_normalsSnapshot = nullptr;
        BaseTexture* m_depthSnapshot = nullptr;
        BaseTexture* m_receiving = nullptr;
        BaseTexture* m_filtered = nullptr;
        BaseTexture* m_horizontal = nullptr;
        uint32_t m_secondaryWidth = 0;
        uint32_t m_secondaryHeight = 0;
        uint32_t m_primaryWidth = 0;
        uint32_t m_primaryHeight = 0;
        uint32_t m_inputWidth = 0;
        uint32_t m_inputHeight = 0;
        bool m_secondaryAttempted = false;
        bool m_usingSecondary = false;
        bool m_logSettings = true;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_depth = nullptr;
        math::Vector4 m_parameters = math::Vector4(0.5f, 0.2f, 1.0f, 0.05f);
        float m_blurDepthThreshold = 0.2f;
        Settings m_settings;
        bool m_blur = true;
    };
}  // namespace nau::render
