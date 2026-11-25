// ambient_occlusion.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once
#include <array>

#include "ao_algorithm.h"
#include "graphics_assets/material_asset.h"
#include "nau/render/render_window.h"

namespace nau::render
{
    /** @brief Render-thread AO pipeline spanning the prepass, late receive and lighting resolve. */
    class AmbientOcclusion final
    {
    public:
        struct Settings
        {
            AmbientOcclusionMode mode = AmbientOcclusionMode::lighting;
            AmbientOcclusionExecution execution = AmbientOcclusionExecution::singleGpu;
            AmbientOcclusionAlgorithm algorithm = AmbientOcclusionAlgorithm::ssao;
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
        /**
         * @brief Applies settings at the start of a frame and prepares the render targets.
         * Changes to the input layout or transport can invalidate the previous AO history.
         * @param width Render target width in pixels.
         * @param height Render target height in pixels.
         * @param settings Requested AO mode, execution and algorithm settings.
         */
        void beginFrame(uint32_t width, uint32_t height, const Settings& settings);
        /**
         * @brief Records AO input preparation and submits secondary work before G-buffer and shadows.
         * @param view View matrix for the inputs.
         * @param projection Projection matrix for the inputs.
         * @param normals Prepass normals, optional when the algorithm only needs depth.
         * @param depth Prepass depth from the same frame as the view and projection.
         */
        void prepareFrame(const math::Matrix4& view, const math::Matrix4& projection, BaseTexture* normals, BaseTexture* depth);
        /**
         * @brief Resolves the AO result after G-buffer and shadows, before lighting consumes it.
         * @param output Framegraph AO target. White represents no occlusion.
         */
        void render(BaseTexture* output);
        void drawDebug(BaseTexture* target, BaseTexture* source);
        bool isDebugView() const;
        math::Vector4 getResolveControl() const;
        const char* getExecutionLabel() const;
        const char* getAlgorithmLabel() const;
        /**
         * @brief Returns the half-resolution AO for lighting resolve.
         * @param fullSize Fallback used when this frame has no internal resolve source.
         * @return A borrowed texture, or fullSize if there is no internal source.
         */
        BaseTexture* getResolveTexture(BaseTexture* fullSize) const;
        void configureResolveSampler(MaterialAssetView& material) const;
        void bindResolveInputs(BaseTexture* fallbackDepth);
        /**
         * @brief Per-frame AO snapshot for benchmark capture.
         * GPU timings and mailbox readbacks may describe older frames than the current frame.
         */
        struct Measurements
        {
            uint64_t frame = 0, aoFrame = 0;
            uint32_t phase = 0;
            bool gpu1 = false, aoValid = false;
            bool reconstructNormals = false;
            nau::render::ao::ExecutionStats gpu1Timing;
            uint64_t gpu1Generation = 0;
            uint32_t aoWidth = 0, aoHeight = 0, inputWidth = 0, inputHeight = 0;
        };
        const Measurements& getMeasurements() const
        {
            return m_measurements;
        }

    private:
        void draw(BaseTexture* target, MaterialAssetView& material, const char* pipeline, BaseTexture* source = nullptr);
        bool drawRaw();
        void resetSecondary();
        void downsampleInputs();
        bool prepareTargets(uint32_t width, uint32_t height);
        bool prepareSecondary();
        void setConstants(MaterialAssetView& material, const char* pipeline, const nau::render::AoConstants& constants);
        void logSettings();
        bool createRandomTexture();

        ao::Algorithm& algorithm() const
        {
            return *m_algorithms[static_cast<size_t>(m_settings.algorithm)];
        }
        std::array<eastl::unique_ptr<ao::Algorithm>, 4> m_algorithms;
        MaterialAssetView::Ptr m_postMaterial;
        ShaderAssetView::Ptr m_vertexShader;
        Texture* m_randomTexture = nullptr;
        d3d::SamplerHandle m_linearClamp = {};
        d3d::SamplerHandle m_linearWrap = {};
        nau::render::ao::SecondaryExecutor* m_secondary = nullptr;
        nau::render::AoConstants m_constants;
        BaseTexture* m_result = nullptr;
        BaseTexture* m_blurredResult = nullptr;
        BaseTexture* m_inputDepth = nullptr;
        BaseTexture* m_inputNormals = nullptr;
        bool m_measureTimings = false;
        bool m_resultInitialized = false;
        uint64_t m_frameIndex = 0;
        Measurements m_measurements;
        uint32_t m_secondaryWidth = 0;
        uint32_t m_secondaryHeight = 0;
        uint32_t m_primaryWidth = 0;
        uint32_t m_primaryHeight = 0;
        bool m_secondaryAttempted = false;
        bool m_usingSecondary = false;
        bool m_logSettings = true;
        BaseTexture* m_normals = nullptr;
        BaseTexture* m_depth = nullptr;
        Settings m_settings;
    };
}  // namespace nau::render
