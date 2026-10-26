// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <atomic>

#include "graphics_assets/material_asset.h"
#include "nau/platform/windows/app/window_message_handler.h"
#include "nau/rtti/rtti_impl.h"

namespace nau::render
{
    class AmbientOcclusionInput final : public IWindowMessageHandler,
                                        public IRttiObject
    {
        NAU_RTTI_CLASS(nau::render::AmbientOcclusionInput, IWindowMessageHandler, IRttiObject)

    public:
        void setTarget(void* windowHandle);
        uint32_t consumeSwitches();
        bool handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        std::atomic<void*> m_windowHandle = nullptr;
        std::atomic<uint32_t> m_switchRequests = 0;
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
        void render(BaseTexture* raw, BaseTexture* horizontal, BaseTexture* output, BaseTexture* normals, BaseTexture* depth);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;

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
        uint32_t m_mode = 1;
        bool m_blur = true;
        bool m_initialized = false;
    };
}  // namespace nau::render
