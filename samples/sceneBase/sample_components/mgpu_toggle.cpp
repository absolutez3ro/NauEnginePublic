// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./mgpu_toggle.h"

#include <algorithm>
#include <cstdio>

#include "nau/input.h"
#include "nau/mgpu/bridge/mgpu_bridge.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(MgpuToggle)

    void MgpuToggle::updateComponent([[maybe_unused]] float dt)
    {
        using namespace nau::input;
        namespace bridge = nau::mgpu::bridge;

        if (isKeyboardButtonPressed(0, input::Key::M))
        {
            bridge::setEnabled(!bridge::isEnabled());
        }
        if (isKeyboardButtonPressed(0, input::Key::V))
        {
            bridge::setVsyncEnabled(!bridge::isVsyncEnabled());
        }

        const bridge::Status status = bridge::getStatus();

        // Bottom-left corner: the other sample windows open in the top-left one.
        const ImVec2 windowSize(470, 300);
        ImGui::SetNextWindowPos(ImVec2(10, std::max(10.0f, ImGui::GetIO().DisplaySize.y - windowSize.y - 10)), ImGuiCond_Once);
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Once);
        ImGui::Begin("MGPU");

        if (!status.linked)
        {
            ImGui::Text("Mode: 1 GPU (built without NAU_ENABLE_MGPU)");
        }
        else if (!status.enabled)
        {
            ImGui::Text("Mode: 1 GPU (MGPU switched off)");
        }
        else if (!status.ready)
        {
            ImGui::Text("Mode: 1 GPU (second GPU not available)");
            ImGui::TextWrapped("%s", status.lastError);
        }
        else
        {
            ImGui::Text("Mode: 2 GPU, shadow cascades: %s",
                        status.shadowsFromSecondGpu ? "second GPU" : "Nau (no atlas from the second GPU)");
            ImGui::Text("Scene GPU:  %s", status.primaryAdapter);
            ImGui::Text("Shadow GPU: %s", status.secondaryAdapter);
            ImGui::Text("Atlases/s: %.1f, latency %.1f ms, atlas age %u frames", status.atlasesPerSecond, status.latencyMs,
                        status.atlasAgeFrames);
            ImGui::Text("Draws: %u, completed %llu, skipped (busy) %llu, failed %llu", status.drawCount,
                        static_cast<unsigned long long>(status.atlasesCompleted),
                        static_cast<unsigned long long>(status.skippedBusy), static_cast<unsigned long long>(status.failed));
        }

        // ImGui's frame rate is the one of the update loop; Nau renders with vsync.
        if (status.linked)
        {
            ImGui::Text("Scene FPS: %.1f, %.2f ms (update loop %.1f)", status.sceneFramesPerSecond,
                        status.sceneFramesPerSecond > 0.0f ? 1000.0f / status.sceneFramesPerSecond : 0.0f, ImGui::GetIO().Framerate);

            // Where the time of a frame goes (averages over 2 s); "-" where nothing was measured.
            const bridge::Status::Timings& t = status.timings;
            const auto ms = [](char (&buffer)[16], const float value) -> const char*
            {
                if (value < 0.0f)
                {
                    return "-";
                }
                std::snprintf(buffer, sizeof(buffer), "%.2f", value);
                return buffer;
            };
            char a[16], b[16], c[16];
            ImGui::Separator();
            ImGui::Text("CPU, ms: frame %.2f, render thread %.2f", t.cpuFrameMs, t.cpuRenderMs);
            ImGui::Text("Lists, ms: main %.2f, cascades %.2f, omni %.2f", t.cpuListsMainMs, t.cpuListsCascadesMs, t.cpuListsOmniMs);
            ImGui::Text("Instances: main %u, cascades %u (%u/%u/%u/%u/%u/%u), omni %u", t.instancesMain, t.instancesCascades,
                        t.instancesPerCascade[0], t.instancesPerCascade[1], t.instancesPerCascade[2], t.instancesPerCascade[3],
                        t.instancesPerCascade[4], t.instancesPerCascade[5], t.instancesOmni);
            ImGui::Text("Second GPU path on the CPU, ms: collect %.2f, submit %.2f", t.cpuCollectMs, t.cpuSubmitMs);
            ImGui::Text("GPU, ms: frame %s, Nau cascades %s, atlas copy %s", ms(a, t.gpuFrameMs), ms(b, t.gpuNauCascadesMs),
                        ms(c, t.gpuAtlasCopyMs));
            ImGui::Text("Second GPU, ms: draw %s, to shared heap %s, transfer %s", ms(a, t.gpu2DrawMs), ms(b, t.gpu2SharedCopyMs),
                        ms(c, t.gpuTransferMs));
        }
        else
        {
            ImGui::Text("Update loop FPS: %.1f", ImGui::GetIO().Framerate);
        }
        if (status.linked)
        {
            ImGui::Text("M - switch MGPU on/off, V - vsync (%s)", bridge::isVsyncEnabled() ? "on" : "off");
        }

        ImGui::End();
    }
}  // namespace nau::sample
