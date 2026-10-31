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
        void beginFrame(const math::Matrix4& view, const math::Matrix4& projection, AmbientOcclusionMode mode);
        void render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, BaseTexture* source = nullptr);
        void drawRaw(BaseTexture* target);
        void drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal);
        void resetSecondary();
        void packNormals(BaseTexture* source);
        void reprojectHistory(BaseTexture* target, BaseTexture* depth);
        bool prepareSecondary(BaseTexture* depth);
        bool receiveSecondary(BaseTexture* target);
        bool publishHistory();

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_rawShader;
        ShaderAssetView::Ptr m_blurShader;
        d3d::mgpu::SsaoProducer* m_secondary = nullptr;
        d3d::mgpu::SsaoConstants m_constants;
        BaseTexture* m_normalsSnapshot = nullptr;
        BaseTexture* m_depthSnapshot = nullptr;
        BaseTexture* m_receiving = nullptr;
        BaseTexture* m_history = nullptr;
        BaseTexture* m_historyDepth = nullptr;
        math::Matrix4 m_viewProjection = math::Matrix4::identity();
        math::Matrix4 m_inverseProjection = math::Matrix4::identity();
        math::Matrix4 m_pendingViewProjection = math::Matrix4::identity();
        math::Matrix4 m_pendingInverseProjection = math::Matrix4::identity();
        math::Matrix4 m_historyViewProjection = math::Matrix4::identity();
        math::Matrix4 m_historyInverseProjection = math::Matrix4::identity();
        uint32_t m_secondaryWidth = 0;
        uint32_t m_secondaryHeight = 0;
        bool m_useSecondary = false;
        bool m_secondaryAttempted = false;
        bool m_historyValid = false;
        bool m_discardPending = false;
        bool m_pendingFrame = false;
        bool m_completedFrameValid = false;
        bool m_synchronization = true;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_depth = nullptr;
        math::Vector4 m_parameters = math::Vector4(0.5f, 0.2f, 1.0f, 0.05f);
        float m_blurDepthThreshold = 0.2f;
        float m_reprojectionDepthThreshold = 0.1f;
        AmbientOcclusionMode m_mode = AmbientOcclusionMode::lighting;
        bool m_blur = true;
        bool m_blurOnSecondary = false;
        bool m_reprojection = true;
    };
}  // namespace nau::render
