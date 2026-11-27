// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <chrono>

#include "nau/async/task.h"
#include "nau/math/math.h"
#include "nau/mgpu/bridge/mgpu_bridge.h"
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"
#include "nau/scene/scene.h"

namespace nau::sample
{
    class SceneSwitcher final : public scene::Component,
                                public scene::IComponentUpdate
    {
    public:
        NAU_OBJECT(SceneSwitcher, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

        using ActiveScenes = eastl::vector<scene::IScene::WeakRef>;
        using ScenesFactory = async::Task<ActiveScenes> (*)();

        enum class SceneSet
        {
            None,
            Default,
            JungleRuins
        };

        static constexpr size_t TimingValueCount = 22;

        void setFactories(ScenesFactory defaultScenes, ScenesFactory jungleRuins);

        async::Task<> switchTo(SceneSet sceneSet);
        bool isBenchmarkRunning() const;

    private:
        void updateComponent(float dt) override;

        struct BenchmarkMode
        {
            double durationSeconds = 0;
            size_t frameCount = 0;
            double frameTimeMs = 0;
            double atlasesPerSecond = 0;
            double latencyMs = 0;
            eastl::array<double, TimingValueCount> timings = {};
            eastl::array<double, TimingValueCount> timingWeights = {};

            double averageFrameTimeMs() const;
            double averageFramesPerSecond() const;
        };

        static constexpr int BenchmarkPhaseCount = 4;
        static constexpr eastl::array<int, BenchmarkPhaseCount> BenchmarkModeIndices = {0, 1, 1, 0};

        void startBenchmark();
        void startBenchmarkPhase(std::chrono::steady_clock::time_point now);
        void updateBenchmark();
        void updateBenchmarkMotion();
        void finishBenchmark();
        bool writeBenchmarkCsv() const;

        ScenesFactory m_defaultScenes = nullptr;
        ScenesFactory m_jungleRuins = nullptr;
        ActiveScenes m_activeScenes;
        SceneSet m_active = SceneSet::None;
        SceneSet m_loading = SceneSet::None;

        math::vec3 m_defaultCameraTranslation = math::vec3::zero();
        math::quat m_defaultCameraRotation = math::quat::identity();
        math::vec3 m_jungleCameraTranslation;
        math::quat m_jungleCameraRotation;
        bool m_jungleCameraSaved = false;

        eastl::array<BenchmarkMode, 2> m_benchmarkModes;
        eastl::array<mgpu::bridge::FrameTimingCapture, BenchmarkPhaseCount> m_frameTimingCaptures;
        mgpu::bridge::Status m_dualGpuStatus;
        int m_benchmarkPhase = -1;
        bool m_capturingFrameTimings = false;
        bool m_benchmarkValid = false;
        bool m_benchmarkDone = false;
        bool m_benchmarkExit = false;
        bool m_savedMgpu = false;
        bool m_savedMgpuAuto = false;
        bool m_savedVsync = true;
        float m_benchmarkLightYaw = 0.0f;
        bool m_startSceneDone = false;

        bool m_benchmarkMotion = false;
        math::vec3 m_motionTranslation = math::vec3::zero();
        math::quat m_motionRotation = math::quat::identity();

        void updateSwitchTest();
        SceneSet m_switchTestSeen = SceneSet::None;
        int m_switchTestSwitches = 0;
        std::chrono::steady_clock::time_point m_switchTestSince;
        std::chrono::steady_clock::time_point m_benchmarkPhaseStart;
        std::chrono::steady_clock::time_point m_benchmarkLastUpdate;
    };
}  // namespace nau::sample
