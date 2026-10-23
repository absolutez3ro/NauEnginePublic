// ambient_occlusion.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <atomic>

#include "graphics_assets/material_asset.h"
#include "nau/3d/multi_gpu_ssao.h"
#include "nau/platform/windows/app/window_message_handler.h"
#include "nau/rtti/rtti_impl.h"

namespace nau::render
{
    class AmbientOcclusionInput final : public IWindowMessageHandler,
                                        public IRttiObject
    {
        NAU_RTTI_CLASS(nau::render::AmbientOcclusionInput, IWindowMessageHandler, IRttiObject)

    public:
        AmbientOcclusionInput() = default;
        AmbientOcclusionInput(const AmbientOcclusionInput&) = delete;
        AmbientOcclusionInput(AmbientOcclusionInput&&) = delete;
        AmbientOcclusionInput& operator=(const AmbientOcclusionInput&) = delete;
        AmbientOcclusionInput& operator=(AmbientOcclusionInput&&) = delete;
        ~AmbientOcclusionInput() override = default;

        void setTarget(void* windowHandle);
        uint32_t consumeSwitches();
        uint32_t consumeDeviceSwitches();
        bool handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        std::atomic<void*> m_windowHandle = nullptr;
        std::atomic<uint32_t> m_switchRequests = 0;
        std::atomic<uint32_t> m_deviceSwitchRequests = 0;
    };

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
        async::Task<> loadAssets();
        void beginFrame(const math::Matrix4& view, const math::Matrix4& projection, void* windowHandle);
        void renderFrame(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* filtered, BaseTexture* normals, BaseTexture* depth, BaseTexture* packedNormals);
        void drawRaw(BaseTexture* target, BaseTexture* normals, BaseTexture* depth);
        void drawBlur(BaseTexture* target, BaseTexture* source, bool horizontal);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        void capturePresentation(BaseTexture* backBuffer) const;
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;

    private:
        void draw(BaseTexture* target, MaterialAssetView& material);
        void packNormals(BaseTexture* target, BaseTexture* normals);
        void capture(BaseTexture* texture, const char* name, uint32_t components) const;
        void captureDepth() const;
        void prepareSecondary(BaseTexture* target);
        void updateDeviceStatus(bool secondary, bool reused = false);
        void cacheResult(BaseTexture* texture);
        bool reuseResult(BaseTexture* texture);

    private:
        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_rawShader;
        ShaderAssetView::Ptr m_blurShader;
        d3d::mgpu::SsaoProducer* m_secondary = nullptr;
        d3d::mgpu::SsaoConstants m_constants;
        void* m_windowHandle = nullptr;
        eastl::string m_deviceStatus;
        uint32_t m_secondaryWidth = 0;
        uint32_t m_secondaryHeight = 0;
        uint32_t m_secondaryFrames = 0;
        uint32_t m_primaryFrames = 0;
        uint32_t m_busyFrames = 0;
        uint32_t m_reusedFrames = 0;
        uint32_t m_historyFrame = 0;
        uint32_t m_busyAtFrame = 0;
        BaseTexture* m_history = nullptr;
        uint32_t m_autoDeviceSwitchFrames = 0;
        uint32_t m_failAtFrame = 0;
        bool m_secondaryAttempted = false;
        bool m_requestedSecondary = true;
        bool m_validateSecondary = false;
        bool m_warpForTesting = false;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_sourceNormals = nullptr;
        BaseTexture* m_depth = nullptr;
        math::Vector4 m_parameters = math::Vector4(0.5f, 0.2f, 1.0f, 0.05f);
        float m_blurDepthThreshold = 0.2f;
        uint32_t m_mode = 1;
        uint32_t m_frame = 0;
        uint32_t m_captureAtFrame = 0;
        uint32_t m_autoSwitchFrames = 0;
        eastl::string m_capturePrefix;
        bool m_blur = true;
        bool m_initialized = false;
    };
}  // namespace nau::render
