// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <chrono>

#include "nau/async/task.h"
#include "nau/math/math.h"
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"
#include "nau/scene/scene.h"

namespace nau::sample
{
    // Key 1 shows the default scenes, key 2 the Jungle Ruins scene (see tools/convert_jungle_ruins.py); the "Scenes" window
    // shows which one is active. The object with this component stays in both: it has the camera and the lights.
    //
    // Key B runs a benchmark in the active scene: without vsync, the shadow cascades in turn on the second GPU and on the
    // scene GPU, the table goes to the window and to the log. With the environment variable NAU_SAMPLE_BENCHMARK set the
    // sample does this by itself and closes (no keys, the window does not need the focus): in Jungle Ruins, or in the
    // default scenes if the value is "default".
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

        // nau::mgpu::bridge::Status::Timings as numbers in the benchmark (scene_switcher.cpp: TIMING_NAMES).
        static constexpr size_t TIMING_VALUES = 22;

        void setFactories(ScenesFactory defaultScenes, ScenesFactory jungleRuins);

        // Replaces the active scenes. Does nothing while another switch is in progress.
        async::Task<> switchTo(SceneSet sceneSet);

    private:
        void updateComponent(float dt) override;

        struct BenchmarkMode
        {
            double time = 0;
            double framesPerSecond = 0;
            double atlasesPerSecond = 0;
            double latencyMs = 0;
            // Time-weighted sums of the timings; a value not measured in a frame has no weight there.
            eastl::array<double, TIMING_VALUES> timings = {};
            eastl::array<double, TIMING_VALUES> timingWeights = {};
            float framesPerSecondAverage() const { return time > 0 ? static_cast<float>(framesPerSecond / time) : 0.0f; }
        };

        void startBenchmark();
        void updateBenchmark();

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

        BenchmarkMode m_benchmarkModes[2]; // [0]: the cascades on the scene GPU, [1]: on the second GPU
        int m_benchmarkPhase = -1; // -1: not running
        bool m_benchmarkDone = false;
        bool m_benchmarkExit = false;
        bool m_savedMgpu = false;
        std::chrono::steady_clock::time_point m_benchmarkPhaseStart;
        std::chrono::steady_clock::time_point m_benchmarkLastUpdate;
    };
}  // namespace nau::sample
