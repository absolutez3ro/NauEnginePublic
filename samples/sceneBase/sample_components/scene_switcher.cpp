// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./scene_switcher.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "nau/app/application.h"
#include "nau/input.h"
#include "nau/io/file_system.h"
#include "nau/mgpu/bridge/mgpu_bridge.h"
#include "nau/samples/components/camera_control.h"
#include "nau/scene/scene_manager.h"
#include "nau/service/service_provider.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(SceneSwitcher)

    namespace
    {
        constexpr const char* JUNGLE_RUINS_FILE = "/content/scenes/jungle_ruins/jungle_ruins.gltf";

        // The camera of the source scene, in the coordinates of the converter (the pyramid is in the origin).
        const math::vec3 JUNGLE_CAMERA_TRANSLATION(29.0f, 2.5f, 43.1f);
        constexpr float JUNGLE_CAMERA_SPEED = 25.0f;
        constexpr float DEFAULT_CAMERA_SPEED = 10.0f;

        // Phases of the benchmark: the second GPU, the scene GPU, and again, so that a drift of the numbers is visible.
        constexpr int BENCHMARK_PHASES = 4;
        constexpr float BENCHMARK_SETTLE_SECONDS = 8.0f; // after a switch: the second GPU loads the meshes, the rate is averaged
        constexpr float BENCHMARK_MEASURE_SECONDS = 16.0f;
        constexpr float BENCHMARK_AUTOSTART_SECONDS = 6.0f;

        // NAU_SAMPLE_BENCHMARK_MOTION=1: the camera sways sideways and turns during the benchmark, so the cascades and the
        // shadows move as in play (a still camera hides how late the shadows of the second GPU are, and lets the shadow
        // cascades stay unchanged). Walking speed: up to 2*pi*amplitude/period = 3 m/s, 12 degrees of turn.
        constexpr float MOTION_AMPLITUDE_METERS = 4.0f;
        constexpr float MOTION_PERIOD_SECONDS = 8.0f;
        constexpr float MOTION_YAW_DEGREES = 12.0f;
        constexpr float MOTION_YAW_PERIOD_SECONDS = 11.0f;

        bool benchmarkMotionRequested()
        {
            const char* const value = std::getenv("NAU_SAMPLE_BENCHMARK_MOTION");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }

        bool isJungleRuinsConverted()
        {
            return getServiceProvider().get<io::IFileSystem>().exists(JUNGLE_RUINS_FILE);
        }

        constexpr const char* TIMING_NAMES[] = {
            "frame CPU", "render thread", "lists main", "lists cascades", "lists omni",
            "instances main", "instances cascades", "instances omni",
            "cascade 0", "cascade 1", "cascade 2", "cascade 3", "cascade 4", "cascade 5",
            "collect", "submit",
            "GPU frame", "GPU Nau cascades", "GPU atlas copy",
            "second GPU draw", "second GPU to shared heap", "transfer"};
        static_assert(std::size(TIMING_NAMES) == SceneSwitcher::TIMING_VALUES);

        eastl::array<float, SceneSwitcher::TIMING_VALUES> timingValues(const nau::mgpu::bridge::Status::Timings& t)
        {
            return {t.cpuFrameMs, t.cpuRenderMs, t.cpuListsMainMs, t.cpuListsCascadesMs, t.cpuListsOmniMs,
                    static_cast<float>(t.instancesMain), static_cast<float>(t.instancesCascades), static_cast<float>(t.instancesOmni),
                    static_cast<float>(t.instancesPerCascade[0]), static_cast<float>(t.instancesPerCascade[1]),
                    static_cast<float>(t.instancesPerCascade[2]), static_cast<float>(t.instancesPerCascade[3]),
                    static_cast<float>(t.instancesPerCascade[4]), static_cast<float>(t.instancesPerCascade[5]),
                    t.cpuCollectMs, t.cpuSubmitMs,
                    t.gpuFrameMs, t.gpuNauCascadesMs, t.gpuAtlasCopyMs,
                    t.gpu2DrawMs, t.gpu2SharedCopyMs, t.gpuTransferMs};
        }
    }  // namespace

    void SceneSwitcher::setFactories(ScenesFactory defaultScenes, ScenesFactory jungleRuins)
    {
        m_defaultScenes = defaultScenes;
        m_jungleRuins = jungleRuins;
    }

    async::Task<> SceneSwitcher::switchTo(const SceneSet sceneSet)
    {
        if (m_loading != SceneSet::None || sceneSet == m_active)
        {
            co_return;
        }
        m_loading = sceneSet;

        scene::ObjectWeakRef<SceneSwitcher> self{*this};

        auto* const camera = getParentObject().findFirstComponent<CameraControl>(true);
        if (camera != nullptr)
        {
            if (m_active != SceneSet::JungleRuins)
            {
                m_defaultCameraTranslation = camera->getPoseTranslation();
                m_defaultCameraRotation = camera->getPoseRotation();
            }
            else
            {
                m_jungleCameraTranslation = camera->getPoseTranslation();
                m_jungleCameraRotation = camera->getPoseRotation();
                m_jungleCameraSaved = true;
            }
        }

        auto& sceneManager = getServiceProvider().get<scene::ISceneManager>();
        for (scene::IScene::WeakRef& sceneRef : m_activeScenes)
        {
            sceneManager.deactivateScene(sceneRef);
        }
        m_activeScenes.clear();
        nau::mgpu::bridge::notifyMeshesChanged();

        ActiveScenes activated = co_await (sceneSet == SceneSet::JungleRuins ? m_jungleRuins() : m_defaultScenes());
        if (!self)
        {
            co_return;
        }

        m_activeScenes = std::move(activated);
        m_active = sceneSet;
        m_loading = SceneSet::None;

        if (camera != nullptr)
        {
            const bool jungle = sceneSet == SceneSet::JungleRuins;
            camera->setTestLightsEnabled(!jungle);
            camera->setStepFactor(jungle ? JUNGLE_CAMERA_SPEED : DEFAULT_CAMERA_SPEED);
            if (!jungle)
            {
                camera->setPose(m_defaultCameraTranslation, m_defaultCameraRotation);
            }
            else if (m_jungleCameraSaved)
            {
                camera->setPose(m_jungleCameraTranslation, m_jungleCameraRotation);
            }
            else
            {
                // Looking at the pyramid: the camera looks along -Z of its rotation.
                const float yaw = std::atan2(static_cast<float>(JUNGLE_CAMERA_TRANSLATION.getX()), static_cast<float>(JUNGLE_CAMERA_TRANSLATION.getZ()));
                camera->setPose(JUNGLE_CAMERA_TRANSLATION, math::quat::rotationY(yaw));
            }
        }
    }

    void SceneSwitcher::startBenchmark()
    {
        m_savedMgpu = nau::mgpu::bridge::isEnabled();
        m_savedMgpuAuto = nau::mgpu::bridge::isAutoEnabled();
        nau::mgpu::bridge::setAutoEnabled(false);
        m_benchmarkModes[0] = {};
        m_benchmarkModes[1] = {};
        m_benchmarkDone = false;
        m_benchmarkPhase = 0;
        m_benchmarkPhaseStart = m_benchmarkLastUpdate = std::chrono::steady_clock::now();
        nau::mgpu::bridge::setVsyncEnabled(false);
        nau::mgpu::bridge::setEnabled(true);

        m_benchmarkMotion = false;
        if (benchmarkMotionRequested())
        {
            if (auto* const camera = getParentObject().findFirstComponent<CameraControl>(true))
            {
                m_benchmarkMotion = true;
                m_motionTranslation = camera->getPoseTranslation();
                m_motionRotation = camera->getPoseRotation();
                m_motionStart = m_benchmarkPhaseStart;
            }
        }
        NAU_LOG_INFO("benchmark: started{}", m_benchmarkMotion ? ", the camera in motion" : "");
    }

    void SceneSwitcher::updateBenchmarkMotion()
    {
        auto* const camera = getParentObject().findFirstComponent<CameraControl>(true);
        if (camera == nullptr)
        {
            return;
        }
        constexpr float TWO_PI = 6.28318531f;
        const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_motionStart).count();
        const float sideways = MOTION_AMPLITUDE_METERS * std::sin(TWO_PI * time / MOTION_PERIOD_SECONDS);
        const float yaw = MOTION_YAW_DEGREES * (TWO_PI / 360.0f) * std::sin(TWO_PI * time / MOTION_YAW_PERIOD_SECONDS);
        camera->setPose(m_motionTranslation + math::vec3(sideways, 0.0f, 0.0f), math::quat::rotationY(yaw) * m_motionRotation);
    }

    void SceneSwitcher::updateBenchmark()
    {
        const auto now = std::chrono::steady_clock::now();
        const float phaseTime = std::chrono::duration<float>(now - m_benchmarkPhaseStart).count();
        const double frameTime = std::chrono::duration<double>(now - m_benchmarkLastUpdate).count();
        m_benchmarkLastUpdate = now;

        const bool mgpu = (m_benchmarkPhase % 2) == 0;
        if (phaseTime > BENCHMARK_SETTLE_SECONDS)
        {
            const nau::mgpu::bridge::Status status = nau::mgpu::bridge::getStatus();
            BenchmarkMode& mode = m_benchmarkModes[mgpu ? 1 : 0];
            mode.time += frameTime;
            mode.framesPerSecond += status.sceneFramesPerSecond * frameTime;
            mode.atlasesPerSecond += status.atlasesPerSecond * frameTime;
            mode.latencyMs += status.latencyMs * frameTime;

            const auto values = timingValues(status.timings);
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (values[i] >= 0.0f)
                {
                    mode.timings[i] += values[i] * frameTime;
                    mode.timingWeights[i] += frameTime;
                }
            }
        }

        if (phaseTime < BENCHMARK_SETTLE_SECONDS + BENCHMARK_MEASURE_SECONDS)
        {
            return;
        }

        ++m_benchmarkPhase;
        m_benchmarkPhaseStart = now;
        if (m_benchmarkPhase < BENCHMARK_PHASES)
        {
            nau::mgpu::bridge::setEnabled((m_benchmarkPhase % 2) == 0);
            return;
        }

        m_benchmarkPhase = -1;
        m_benchmarkDone = true;
        nau::mgpu::bridge::setEnabled(m_savedMgpu);
        nau::mgpu::bridge::setAutoEnabled(m_savedMgpuAuto);
        nau::mgpu::bridge::setVsyncEnabled(true);
        if (m_benchmarkMotion)
        {
            m_benchmarkMotion = false;
            if (auto* const camera = getParentObject().findFirstComponent<CameraControl>(true))
            {
                camera->setPose(m_motionTranslation, m_motionRotation);
            }
        }

        const BenchmarkMode& nauMode = m_benchmarkModes[0];
        const BenchmarkMode& gpu2Mode = m_benchmarkModes[1];
        const float nauFps = nauMode.framesPerSecondAverage();
        const float gpu2Fps = gpu2Mode.framesPerSecondAverage();
        NAU_LOG_INFO("benchmark: cascades on the scene GPU: {:.1f} fps, {:.2f} ms", nauFps, 1000.0f / std::max(nauFps, 0.001f));
        NAU_LOG_INFO("benchmark: cascades on the second GPU: {:.1f} fps, {:.2f} ms, {:.1f} atlases/s, atlas latency {:.1f} ms, gain {:+.1f}%", gpu2Fps,
                     1000.0f / std::max(gpu2Fps, 0.001f), gpu2Mode.time > 0 ? gpu2Mode.atlasesPerSecond / gpu2Mode.time : 0.0,
                     gpu2Mode.time > 0 ? gpu2Mode.latencyMs / gpu2Mode.time : 0.0, (gpu2Fps / std::max(nauFps, 0.001f) - 1.0f) * 100.0f);

        // Where the time goes in each mode (ms, instances: per frame; GPU passes: per measured pass).
        for (const auto& [mode, name] : {std::pair{&nauMode, "scene GPU"}, std::pair{&gpu2Mode, "second GPU"}})
        {
            std::string text;
            for (size_t i = 0; i < TIMING_VALUES; ++i)
            {
                text += ::fmt::format("{}{} ", i == 0 ? "" : ", ", TIMING_NAMES[i]);
                text += mode->timingWeights[i] > 0 ? ::fmt::format("{:.2f}", mode->timings[i] / mode->timingWeights[i]) : std::string("-");
            }
            NAU_LOG_INFO("benchmark timing, cascades on the {}: {}", name, text);
        }

        if (m_benchmarkExit)
        {
            getApplication().stop();
        }
    }

    void SceneSwitcher::updateSwitchTest()
    {
        static const int switches = []
        {
            const char* const value = std::getenv("NAU_SAMPLE_SWITCH_TEST");
            return value != nullptr ? std::max(0, std::atoi(value)) : 0;
        }();
        if (switches == 0 || m_loading != SceneSet::None || m_active == SceneSet::None || !isJungleRuinsConverted())
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_switchTestSeen != m_active)
        {
            m_switchTestSeen = m_active;
            m_switchTestSince = now;
            NAU_LOG_INFO("switch test: active {}", m_active == SceneSet::JungleRuins ? "jungle" : "default");
        }
        else if (std::chrono::duration<float>(now - m_switchTestSince).count() > 3.0f)
        {
            m_switchTestSince = now;
            if (m_switchTestSwitches++ < switches)
            {
                switchTo(m_active == SceneSet::Default ? SceneSet::JungleRuins : SceneSet::Default).detach();
            }
            else
            {
                NAU_LOG_INFO("switch test: done, {} switches", switches);
                getApplication().stop();
            }
        }
    }

    void SceneSwitcher::updateComponent([[maybe_unused]] float dt)
    {
        updateSwitchTest();
        using namespace nau::input;

        static const bool converted = isJungleRuinsConverted();
        static const char* const autoBenchmarkValue = std::getenv("NAU_SAMPLE_BENCHMARK");
        static const bool autoBenchmark = autoBenchmarkValue != nullptr;
        static const SceneSet autoBenchmarkScene = autoBenchmark && std::strcmp(autoBenchmarkValue, "default") == 0 ? SceneSet::Default : SceneSet::JungleRuins;

        // NAU_SAMPLE_SCENE=jungle: Jungle Ruins once the default scenes are up, without a benchmark (the auto mode of Mgpu
        // works only outside of one).
        static const char* const startSceneValue = std::getenv("NAU_SAMPLE_SCENE");
        if (!m_startSceneDone && startSceneValue != nullptr && std::strcmp(startSceneValue, "jungle") == 0 && converted &&
            m_active == SceneSet::Default && m_loading == SceneSet::None)
        {
            m_startSceneDone = true;
            switchTo(SceneSet::JungleRuins).detach();
        }

        if (autoBenchmark && !m_benchmarkDone && m_benchmarkPhase < 0 && m_loading == SceneSet::None)
        {
            if (m_active == SceneSet::Default && autoBenchmarkScene == SceneSet::JungleRuins && converted)
            {
                switchTo(SceneSet::JungleRuins).detach();
            }
            else if (m_active == autoBenchmarkScene)
            {
                const auto now = std::chrono::steady_clock::now();
                if (m_benchmarkPhaseStart == std::chrono::steady_clock::time_point{})
                {
                    m_benchmarkPhaseStart = now;
                }
                else if (std::chrono::duration<float>(now - m_benchmarkPhaseStart).count() > BENCHMARK_AUTOSTART_SECONDS)
                {
                    m_benchmarkExit = true;
                    startBenchmark();
                }
            }
        }

        if (m_benchmarkPhase >= 0)
        {
            if (m_benchmarkMotion)
            {
                updateBenchmarkMotion();
            }
            updateBenchmark();
        }
        else if (m_loading == SceneSet::None && isKeyboardButtonPressed(0, input::Key::B))
        {
            startBenchmark();
        }

        if (m_loading == SceneSet::None && m_benchmarkPhase < 0)
        {
            if (isKeyboardButtonPressed(0, input::Key::N1))
            {
                switchTo(SceneSet::Default).detach();
            }
            else if (isKeyboardButtonPressed(0, input::Key::N2) && converted)
            {
                switchTo(SceneSet::JungleRuins).detach();
            }
        }

        ImGui::SetNextWindowPos(ImVec2(std::max(10.0f, ImGui::GetIO().DisplaySize.x - 330.0f), 210.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 150), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenes");
        ImGui::Text("1 - default scene%s", m_active == SceneSet::Default ? " (active)" : "");
        if (converted)
        {
            ImGui::Text("2 - Jungle Ruins%s", m_active == SceneSet::JungleRuins ? " (active)" : "");
        }
        else
        {
            ImGui::TextWrapped("2 - Jungle Ruins is not converted: tools/convert_jungle_ruins.py");
        }
        if (m_loading != SceneSet::None)
        {
            ImGui::Text("Loading...");
        }
        if (m_benchmarkPhase >= 0)
        {
            ImGui::Text("Benchmark: phase %d of %d", m_benchmarkPhase + 1, BENCHMARK_PHASES);
        }
        else if (m_benchmarkDone)
        {
            const float nauFps = m_benchmarkModes[0].framesPerSecondAverage();
            const float gpu2Fps = m_benchmarkModes[1].framesPerSecondAverage();
            ImGui::Text("Cascades on the scene GPU:  %.1f FPS", nauFps);
            ImGui::Text("Cascades on the second GPU: %.1f FPS", gpu2Fps);
            ImGui::Text("Gain: %+.1f%%", (gpu2Fps / std::max(nauFps, 0.001f) - 1.0f) * 100.0f);
        }
        else
        {
            ImGui::Text("B - benchmark of the active scene");
        }
        ImGui::End();
    }
}  // namespace nau::sample
