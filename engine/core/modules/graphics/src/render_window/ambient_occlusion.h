// ambient_occlusion.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once
#include <array>
#include <chrono>
#include <deque>

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
            AmbientOcclusionPipeline pipeline;
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
        void beginFrame(uint32_t width, uint32_t height, const Settings& settings);
        void prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth);
        void render(BaseTexture* output);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;
        const char* getExecutionLabel() const;
        const char* getVariantLabel() const;
        BaseTexture* getResolveTexture(BaseTexture* fullSize) const;
        void configureResolveSampler(MaterialAssetView& material) const;
        void bindResolveInputs();
        float getWaitMilliseconds() const { return m_usingSecondary && m_settings.pipeline.waitForSecondary && m_settings.mode != AmbientOcclusionMode::off ? m_waitMilliseconds : -1.f; }
        const char* getWaitLabel() const;
        struct Measurements
        {
            uint64_t frame = 0, aoFrame = 0, timingFrame = 0, chainFrame = 0;
            uint32_t phase = 0, loadMask = 0;
            bool gpu1 = false, aoValid = false;
            int readyAtConsumer = -1;
            float overlapMs = -1.f, waitMs = -1.f, chainObservedMs = -1.f;
            d3d::mgpu::SsaoGpuTiming gpu1Timing;
        };
        const Measurements& getMeasurements() const { return m_measurements; }

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source = nullptr);
        void filter(BaseTexture* output);
        void resetSecondary();
        void downsampleInputs();
        bool prepareTargets(uint32_t width, uint32_t height);
        bool prepareSecondary();
        void setConstants(MaterialAssetView& material, const char* pipeline);
        void logSettings();
        bool createRandomTexture();
        void pollWaitTimings();
        void waitForSecondary();
        void beginOverlapTiming();

        MaterialAssetView::Ptr m_rawMaterial;
        MaterialAssetView::Ptr m_blurMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        ShaderAssetView::Ptr m_rawShader;
        ShaderAssetView::Ptr m_filterShader;
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
        BaseTexture* m_fullOutput = nullptr;
        BaseTexture* m_resolveSource = nullptr;
        BaseTexture* m_asyncRaw = nullptr;
        BaseTexture* m_asyncDepth = nullptr;
        BaseTexture* m_asyncNormals = nullptr;
        BaseTexture* m_drawRaw = nullptr;
        BaseTexture* m_drawDepth = nullptr;
        BaseTexture* m_drawNormals = nullptr;
        bool m_asyncValid = false;
        d3d::mgpu::SsaoConstants m_submittedConstants;
        d3d::mgpu::SsaoConstants m_asyncConstants;
        struct WaitTiming
        {
            void* begin = nullptr;
            void* end = nullptr;
            void* overlapStart = nullptr;
            bool pending = false;
            bool waited = true;
            int readyAtConsumer = -1;
            uint64_t frame = 0;
            uint64_t generation = 0;
            uint64_t sequence = 0;
        };
        std::array<WaitTiming, 16> m_waitTimings;
        uint64_t m_timingGeneration = 0;
        uint64_t m_nextWaitSequence = 0;
        uint64_t m_lastWaitSequence = 0;
        float m_waitMilliseconds = -1.f;
        WaitTiming* m_currentTiming = nullptr;
        bool m_measureTimings = false;
        uint64_t m_frameIndex = 0, m_submittedFrame = 0, m_asyncFrame = 0;
        Measurements m_measurements;
        struct PendingCompletion
        {
            d3d::mgpu::PrimarySyncPoint point;
            uint64_t frame;
            std::chrono::steady_clock::time_point submitted;
        };
        std::deque<PendingCompletion> m_pendingCompletions;
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
    };
}  // namespace nau::render
