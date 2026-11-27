// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "./scene_switcher.h"
#include "./light_control.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
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

        const math::vec3 JUNGLE_CAMERA_TRANSLATION(29.0f, 2.5f, 43.1f);
        constexpr float JUNGLE_CAMERA_SPEED = 25.0f;
        constexpr float DEFAULT_CAMERA_SPEED = 10.0f;

        constexpr float BENCHMARK_SETTLE_SECONDS = 8.0f;
        constexpr float BENCHMARK_MEASURE_SECONDS = 16.0f;
        constexpr float BENCHMARK_AUTOSTART_SECONDS = 6.0f;

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
        static_assert(std::size(TIMING_NAMES) == SceneSwitcher::TimingValueCount);

        eastl::array<float, SceneSwitcher::TimingValueCount> timingValues(const nau::mgpu::bridge::Status::Timings& t)
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
                const float yaw = std::atan2(static_cast<float>(JUNGLE_CAMERA_TRANSLATION.getX()), static_cast<float>(JUNGLE_CAMERA_TRANSLATION.getZ()));
                camera->setPose(JUNGLE_CAMERA_TRANSLATION, math::quat::rotationY(yaw));
            }
        }
    }

    bool SceneSwitcher::isBenchmarkRunning() const
    {
        return m_benchmarkPhase >= 0;
    }

    double SceneSwitcher::BenchmarkMode::averageFrameTimeMs() const
    {
        return frameCount > 0 ? frameTimeMs / static_cast<double>(frameCount) : 0.0;
    }

    double SceneSwitcher::BenchmarkMode::averageFramesPerSecond() const
    {
        return frameTimeMs > 0.0 ? 1000.0 * static_cast<double>(frameCount) / frameTimeMs : 0.0;
    }

    void SceneSwitcher::startBenchmark()
    {
        if (!nau::mgpu::bridge::getStatus().linked)
        {
            m_benchmarkDone = true;
            NAU_LOG_ERROR("benchmark: SceneBaseSample needs a build with NAU_ENABLE_MGPU");
            if (m_benchmarkExit)
            {
                getApplication().stop();
            }
            return;
        }

        m_savedMgpu = nau::mgpu::bridge::isEnabled();
        m_savedMgpuAuto = nau::mgpu::bridge::isAutoEnabled();
        m_savedVsync = nau::mgpu::bridge::isVsyncEnabled();
        nau::mgpu::bridge::setAutoEnabled(false);
        nau::mgpu::bridge::setVsyncEnabled(false);
        m_benchmarkModes = {};
        m_frameTimingCaptures = {};
        m_dualGpuStatus = {};
        m_benchmarkDone = false;
        m_benchmarkValid = false;
        m_benchmarkPhase = 0;
        if (const auto* lightControl = getParentObject().findFirstComponent<LightControl>())
        {
            m_benchmarkLightYaw = lightControl->getYawDegrees();
        }

        m_benchmarkMotion = false;
        if (benchmarkMotionRequested())
        {
            if (auto* const camera = getParentObject().findFirstComponent<CameraControl>(true))
            {
                m_benchmarkMotion = true;
                m_motionTranslation = camera->getPoseTranslation();
                m_motionRotation = camera->getPoseRotation();
            }
        }
        NAU_LOG_INFO("benchmark: started{}", m_benchmarkMotion ? ", the camera in motion" : "");
        startBenchmarkPhase(std::chrono::steady_clock::now());
    }

    void SceneSwitcher::startBenchmarkPhase(const std::chrono::steady_clock::time_point now)
    {
        m_benchmarkPhaseStart = m_benchmarkLastUpdate = now;
        if (auto* lightControl = getParentObject().findFirstComponent<LightControl>())
        {
            lightControl->setYawDegrees(m_benchmarkExit ? 0.0f : m_benchmarkLightYaw);
        }
        nau::mgpu::bridge::setEnabled(BenchmarkModeIndices[m_benchmarkPhase] == 1);
    }

    void SceneSwitcher::updateBenchmarkMotion()
    {
        auto* const camera = getParentObject().findFirstComponent<CameraControl>(true);
        if (camera == nullptr)
        {
            return;
        }
        constexpr float TWO_PI = 6.28318531f;
        const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_benchmarkPhaseStart).count();
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

        if (phaseTime < BENCHMARK_SETTLE_SECONDS)
        {
            return;
        }
        if (!m_capturingFrameTimings)
        {
            nau::mgpu::bridge::startFrameTimingCapture();
            m_capturingFrameTimings = true;
            return;
        }

        const int modeIndex = BenchmarkModeIndices[m_benchmarkPhase];
        BenchmarkMode& mode = m_benchmarkModes[modeIndex];
        const auto status = nau::mgpu::bridge::getStatus();
        mode.durationSeconds += frameTime;
        mode.atlasesPerSecond += status.atlasesPerSecond * frameTime;
        mode.latencyMs += status.latencyMs * frameTime;

        const auto values = timingValues(status.timings);
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (values[index] >= 0.0f)
            {
                mode.timings[index] += values[index] * frameTime;
                mode.timingWeights[index] += frameTime;
            }
        }

        if (phaseTime < BENCHMARK_SETTLE_SECONDS + BENCHMARK_MEASURE_SECONDS)
        {
            return;
        }

        auto& capture = m_frameTimingCaptures[m_benchmarkPhase];
        capture = nau::mgpu::bridge::stopFrameTimingCapture();
        m_capturingFrameTimings = false;
        if (modeIndex == 1)
        {
            m_dualGpuStatus = status;
        }
        mode.frameCount += capture.frames.size();
        for (const auto& frame : capture.frames)
        {
            mode.frameTimeMs += frame.frameTimeMs;
        }

        if (++m_benchmarkPhase < BenchmarkPhaseCount)
        {
            startBenchmarkPhase(std::chrono::steady_clock::now());
            return;
        }
        finishBenchmark();
    }

    void SceneSwitcher::finishBenchmark()
    {
        m_benchmarkPhase = -1;
        m_benchmarkDone = true;
        const auto& status = m_dualGpuStatus;
        const bool csvWritten = writeBenchmarkCsv();
        nau::mgpu::bridge::setEnabled(m_savedMgpu);
        nau::mgpu::bridge::setAutoEnabled(m_savedMgpuAuto);
        nau::mgpu::bridge::setVsyncEnabled(m_savedVsync);
        if (auto* lightControl = getParentObject().findFirstComponent<LightControl>())
        {
            lightControl->setYawDegrees(m_benchmarkLightYaw);
        }
        if (m_benchmarkMotion)
        {
            m_benchmarkMotion = false;
            if (auto* const camera = getParentObject().findFirstComponent<CameraControl>(true))
            {
                camera->setPose(m_motionTranslation, m_motionRotation);
            }
        }

        const BenchmarkMode& singleGpuMode = m_benchmarkModes[0];
        const BenchmarkMode& dualGpuMode = m_benchmarkModes[1];
        bool modesValid = status.ready;
        for (size_t phase = 0; phase < m_frameTimingCaptures.size(); ++phase)
        {
            const auto& capture = m_frameTimingCaptures[phase];
            const bool expectedSecondGpu = BenchmarkModeIndices[phase] == 1;
            const bool atlasSourcesMatch = std::all_of(capture.frames.begin(), capture.frames.end(), [expectedSecondGpu](const auto& frame)
            {
                return frame.cascadesFromSecondGpu == expectedSecondGpu && (expectedSecondGpu || !frame.omniShadowsFromSecondGpu);
            });
            modesValid = modesValid && !capture.overflowed && !capture.frames.empty() && atlasSourcesMatch;
        }
        m_benchmarkValid = csvWritten && modesValid;
        if (m_benchmarkValid)
        {
            const auto [width, height] = getServiceProvider().get<IWindowManager>().getActiveWindow().getClientSize();
            NAU_LOG_INFO("benchmark: adapters {} / {}", status.primaryAdapter, status.secondaryAdapter);
            NAU_LOG_INFO("benchmark: viewport {}x{}", width, height);
            NAU_LOG_INFO("benchmark: 1 GPU {:.2f} ms ({:.1f} FPS), 2 GPUs {:.2f} ms ({:.1f} FPS)",
                         singleGpuMode.averageFrameTimeMs(), singleGpuMode.averageFramesPerSecond(),
                         dualGpuMode.averageFrameTimeMs(), dualGpuMode.averageFramesPerSecond());
            if (dualGpuMode.durationSeconds > 0.0)
            {
                NAU_LOG_INFO("benchmark: {:.1f} atlases/s, atlas latency {:.1f} ms",
                             dualGpuMode.atlasesPerSecond / dualGpuMode.durationSeconds, dualGpuMode.latencyMs / dualGpuMode.durationSeconds);
            }
        }
        else if (!modesValid)
        {
            NAU_LOG_ERROR("benchmark: incomplete frame capture or unexpected shadow atlas source: {}", status.lastError);
        }

        for (const auto& [mode, name] : {
                 std::pair{&singleGpuMode,  "scene GPU"},
                 std::pair{  &dualGpuMode, "second GPU"}
        })
        {
            std::string text;
            for (size_t i = 0; i < TimingValueCount; ++i)
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

    bool SceneSwitcher::writeBenchmarkCsv() const
    {
        const char* configuredPath = std::getenv("NAU_SAMPLE_BENCHMARK_CSV");
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const std::string outputPath = configuredPath && *configuredPath
                                           ? configuredPath
                                           : ::fmt::format("mgpu_benchmark_{}.csv", std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        output.imbue(std::locale::classic());
        output << std::fixed << std::setprecision(6);
        output << "scene,phase,gpu_count,frame,elapsed_s,frame_time_ms,cascades_from_second_gpu,omni_shadows_from_second_gpu,cascade_atlas_age_frames,capture_overflow\n";
        for (size_t phase = 0; phase < m_frameTimingCaptures.size(); ++phase)
        {
            const auto& capture = m_frameTimingCaptures[phase];
            double elapsedMs = 0.0;
            for (size_t frameIndex = 0; frameIndex < capture.frames.size(); ++frameIndex)
            {
                const auto& frame = capture.frames[frameIndex];
                elapsedMs += frame.frameTimeMs;
                output << (m_active == SceneSet::JungleRuins ? "jungle" : "default") << ',' << phase + 1 << ',' << BenchmarkModeIndices[phase] + 1 << ','
                       << frameIndex + 1 << ',' << elapsedMs / 1000.0 << ',' << frame.frameTimeMs << ',' << frame.cascadesFromSecondGpu << ','
                       << frame.omniShadowsFromSecondGpu << ',' << frame.cascadeAtlasAgeFrames << ',' << capture.overflowed << '\n';
            }
        }
        output.close();
        if (!output)
        {
            NAU_LOG_ERROR("benchmark: failed to write {}", outputPath);
            return false;
        }

        NAU_LOG_INFO("benchmark: wrote {}", outputPath);
        return true;
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
        using namespace nau::input;

        static const bool converted = isJungleRuinsConverted();
        static const char* const autoBenchmarkValue = std::getenv("NAU_SAMPLE_BENCHMARK");
        static const bool autoBenchmark = autoBenchmarkValue != nullptr;
        static const SceneSet autoBenchmarkScene = autoBenchmark && std::strcmp(autoBenchmarkValue, "default") == 0 ? SceneSet::Default : SceneSet::JungleRuins;

        if (!autoBenchmark && !isBenchmarkRunning())
        {
            updateSwitchTest();
        }
        static const char* const startSceneValue = std::getenv("NAU_SAMPLE_SCENE");
        if (!autoBenchmark && !m_startSceneDone && startSceneValue != nullptr && std::strcmp(startSceneValue, "jungle") == 0 && converted &&
            m_active == SceneSet::Default && m_loading == SceneSet::None)
        {
            m_startSceneDone = true;
            switchTo(SceneSet::JungleRuins).detach();
        }

        if (autoBenchmark && !m_benchmarkDone && m_benchmarkPhase < 0 && m_loading == SceneSet::None)
        {
            if (m_active == SceneSet::Default && autoBenchmarkScene == SceneSet::JungleRuins)
            {
                if (!converted)
                {
                    m_benchmarkDone = true;
                    NAU_LOG_ERROR("benchmark: Jungle Ruins is missing ({})", JUNGLE_RUINS_FILE);
                    getApplication().stop();
                    return;
                }
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
        else if (!autoBenchmark && m_active != SceneSet::None && m_loading == SceneSet::None && isKeyboardButtonPressed(0, input::Key::B))
        {
            startBenchmark();
        }

        if (!autoBenchmark && m_loading == SceneSet::None && !isBenchmarkRunning())
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
            ImGui::Text("Benchmark: phase %d of %d", m_benchmarkPhase + 1, BenchmarkPhaseCount);
        }
        else if (m_benchmarkValid)
        {
            ImGui::Text("1 GPU: %.2f ms (%.1f FPS)", m_benchmarkModes[0].averageFrameTimeMs(), m_benchmarkModes[0].averageFramesPerSecond());
            ImGui::Text("2 GPUs: %.2f ms (%.1f FPS)", m_benchmarkModes[1].averageFrameTimeMs(), m_benchmarkModes[1].averageFramesPerSecond());
        }
        else if (m_benchmarkDone)
        {
            ImGui::TextUnformatted("Benchmark failed. See the log.");
        }
        else
        {
            ImGui::Text("B - benchmark of the active scene");
        }
        ImGui::End();
    }
}  // namespace nau::sample
