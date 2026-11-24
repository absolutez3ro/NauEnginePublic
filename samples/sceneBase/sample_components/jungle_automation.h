// jungle_automation.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.

#pragma once

#include <chrono>

#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"
#include "nau/scene/scene_object.h"

namespace nau::sample
{
    class JungleAutomation final : public scene::Component, public scene::IComponentUpdate
    {
        NAU_OBJECT(JungleAutomation, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

    public:
        /**
         * @brief Configures the camera and timing of an automated AO capture.
         * @param camera Camera to animate during the capture.
         */
        void initialize(scene::SceneObject& camera);

    private:
        void updateComponent(float dt) override;

        scene::ObjectWeakRef<scene::SceneObject> m_camera;
        math::Transform m_cameraStart;
        std::chrono::steady_clock::time_point m_start;
        float m_measurementStart = 0.f;
        float m_measureSeconds = 0.f;
        uint32_t m_phase = 0;
        bool m_cameraMotion = true;
        bool m_lifecycleChecks = false;
        uint32_t m_lifecycleStep = UINT32_MAX;
        async::Task<> m_resize;
    };
}
