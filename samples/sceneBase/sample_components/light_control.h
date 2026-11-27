// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"

namespace nau::sample
{
    class LightControl final : public scene::Component,
                               public scene::IComponentUpdate
    {
    public:
        NAU_OBJECT(LightControl, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

        float getYawDegrees() const;
        void setYawDegrees(float yawDegrees);

    private:
        void updateComponent(float dt) override;

        float m_yawDegrees = 0.0f;
        float m_pitchDegrees = -45.0f;
        float m_intensity = 1.0f;
        bool m_castShadows = true;
        bool m_autoRotate = false;
        float m_rotationSpeed = 20.0f;
        bool m_initialized = false;
    };
}  // namespace nau::sample
