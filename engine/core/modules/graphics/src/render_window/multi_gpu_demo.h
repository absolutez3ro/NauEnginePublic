// multi_gpu_demo.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <atomic>
#include <chrono>

#include "graphics_assets/material_asset.h"
#include "nau/3d/multi_gpu_fullscreen.h"
#include "nau/platform/windows/app/window_message_handler.h"
#include "nau/rtti/rtti_impl.h"

namespace nau::render
{
    /** @brief Latches window key events so a complete tap between two slow WARP frames is retained. */
    class MultiGpuDemoInput final : public IWindowMessageHandler,
                                    public IRttiObject
    {
        NAU_RTTI_CLASS(nau::render::MultiGpuDemoInput, IWindowMessageHandler, IRttiObject)

    public:
        void setTarget(void* windowHandle);
        bool consumeSwitch();
        bool handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        std::atomic<void*> m_windowHandle = nullptr;
        std::atomic<uint32_t> m_switchRequests = 0;
    };

    /** @brief Optional SceneBaseSample demonstration, owned and called exclusively by its render window. */
    class MultiGpuDemo final
    {
    public:
        MultiGpuDemo() = default;
        MultiGpuDemo(const MultiGpuDemo&) = delete;
        MultiGpuDemo(MultiGpuDemo&&) = delete;
        MultiGpuDemo& operator=(const MultiGpuDemo&) = delete;
        MultiGpuDemo& operator=(MultiGpuDemo&&) = delete;
        ~MultiGpuDemo();

        static bool isRequested();
        async::Task<> loadAssets();
        void render(BaseTexture* backBuffer, void* windowHandle);

    private:
        bool initialize(void* windowHandle);
        void drawOnPrimary(const math::Vector4& constants);
        void updateStatus(const char* mode, void* windowHandle);
        void captureFrame();

        MaterialAssetView::Ptr m_material;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_pixelShader;
        BaseTexture* m_destination = nullptr;
        d3d::mgpu::FullscreenProducer* m_producer = nullptr;
        std::chrono::steady_clock::time_point m_start;
        std::chrono::steady_clock::time_point m_lastTransfer;
        uint64_t m_frame = 0;
        uint64_t m_transferredFrames = 0;
        uint64_t m_primaryFrames = 0;
        uint32_t m_visiblePhase = 0;
        uint32_t m_autoSwitchFrames = 0;
        uint32_t m_failAtFrame = 0;
        uint32_t m_captureAtFrame = 0;
        eastl::string m_capturePath;
        eastl::string m_status;
        bool m_initialized = false;
        bool m_requestedSecondary = true;
        bool m_hasSecondaryFrame = false;
        bool m_failureInjected = false;
        bool m_warpForTesting = false;

        // The demonstration uses a fixed payload; resizing the window only changes the presentation scale.
        static constexpr uint32_t WIDTH = 960;
        static constexpr uint32_t HEIGHT = 540;
    };
}  // namespace nau::render
