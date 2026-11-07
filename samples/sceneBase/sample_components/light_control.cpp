// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./light_control.h"

#include <algorithm>
#include <cmath>

#include "nau/math/math.h"
#include "nau/scene/components/directional_light_component.h"
#include "nau/scene/scene_object.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(LightControl)

    void LightControl::updateComponent(float dt)
    {
        using namespace nau::math;

        auto* const light = getParentObject().findFirstComponent<scene::DirectionalLightComponent>();
        if (light == nullptr)
        {
            return;
        }

        constexpr float kPitchMin = -89.0f;
        constexpr float kPitchMax = -5.0f;

        if (!m_initialized)
        {
            m_castShadows = light->hasShadows();
            m_intensity = light->getIntensity();
            m_initialized = true;
        }

        if (m_autoRotate)
        {
            m_yawDegrees += m_rotationSpeed * dt;
            if (m_yawDegrees > 180.0f)
            {
                m_yawDegrees -= 360.0f;
            }
            else if (m_yawDegrees < -180.0f)
            {
                m_yawDegrees += 360.0f;
            }
        }

        // Bottom-right area: the other sample windows are on the left.
        ImGui::SetNextWindowPos(ImVec2(std::max(10.0f, ImGui::GetIO().DisplaySize.x - 330.0f), 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 190), ImGuiCond_FirstUseEver);
        ImGui::Begin("Directional light");

        ImGui::SliderFloat("Yaw", &m_yawDegrees, -180.0f, 180.0f, "%.1f deg");
        ImGui::SliderFloat("Pitch", &m_pitchDegrees, kPitchMin, kPitchMax, "%.1f deg");
        ImGui::SliderFloat("Intensity", &m_intensity, 0.0f, 10.0f);
        ImGui::Checkbox("Cast shadows", &m_castShadows);
        ImGui::Checkbox("Auto rotate", &m_autoRotate);
        ImGui::SliderFloat("Speed", &m_rotationSpeed, -90.0f, 90.0f, "%.0f deg/s");

        m_pitchDegrees = std::clamp(m_pitchDegrees, kPitchMin, kPitchMax);

        const float yaw = m_yawDegrees * 3.14159265358979f / 180.0f;
        const float pitch = m_pitchDegrees * 3.14159265358979f / 180.0f;
        const Vector3 direction(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));

        ImGui::Text("Ray direction: (%.2f, %.2f, %.2f)", static_cast<float>(direction.getX()), static_cast<float>(direction.getY()),
                    static_cast<float>(direction.getZ()));
        ImGui::TextWrapped("The sun stays above the horizon (pitch up to -5 deg).");
        ImGui::End();

        // DirectionalLightComponent::getDirection() is the rotation applied to (1, 0, 0).
        light->setRotation(Quat::rotation(Vector3(1.0f, 0.0f, 0.0f), direction));
        light->setIntensity(m_intensity);
        light->setCastShadows(m_castShadows);
    }
}  // namespace nau::sample
