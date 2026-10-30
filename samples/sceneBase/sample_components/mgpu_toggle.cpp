// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./mgpu_toggle.h"

#include <algorithm>

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

        const bridge::Status status = bridge::getStatus();

        // Bottom-left corner: the other sample windows open in the top-left one.
        const ImVec2 windowSize(380, 150);
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
            ImGui::Text("Mode: 2 GPU");
            ImGui::Text("Scene GPU:  %s", status.primaryAdapter);
            ImGui::Text("Second GPU: %s", status.secondaryAdapter);
            ImGui::Text("Second GPU images/s: %.1f, latency %.1f ms", status.imagesPerSecond, status.latencyMs);
            ImGui::Text("Images OK / failed: %llu / %llu", static_cast<unsigned long long>(status.imagesOk),
                        static_cast<unsigned long long>(status.imagesFailed));
        }

        ImGui::Text("Scene FPS: %.1f", ImGui::GetIO().Framerate);
        if (status.linked)
        {
            ImGui::Text("M - switch MGPU on/off");
        }

        ImGui::End();
    }
}  // namespace nau::sample
