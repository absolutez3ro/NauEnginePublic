// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"

namespace nau::sample
{
    // The "Directional light" window: direction (yaw / pitch), intensity, shadows on/off and an automatic rotation of
    // the direction, to check the shadow cascades with the sun in different places. A directional light has no
    // position: its shadows depend on the direction only.
    class LightControl final : public scene::Component,
                               public scene::IComponentUpdate
    {
    public:
        NAU_OBJECT(LightControl, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

    private:
        void updateComponent(float dt) override;

        // Direction of the light rays: (cos(pitch)cos(yaw), sin(pitch), cos(pitch)sin(yaw)). The sun has to stay above
        // the horizon (pitch < 0, the rays go down), otherwise the cascades are degenerate.
        float m_yawDegrees = 0.0f;
        float m_pitchDegrees = -45.0f;
        float m_intensity = 1.0f;
        bool m_castShadows = true;
        bool m_autoRotate = false;
        float m_rotationSpeed = 20.0f;  // degrees per second
        bool m_initialized = false;
    };
}  // namespace nau::sample
