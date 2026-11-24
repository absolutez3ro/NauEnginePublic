// Copyright 2026 N-GINN LLC. All rights reserved.

#include "jungle_automation.h"

#include <EASTL/algorithm.h>

#include <cmath>

#include "nau/app/application.h"
#include "nau/app/global_properties.h"
#include "nau/service/service_provider.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(JungleAutomation)

    void JungleAutomation::initialize(scene::SceneObject& camera)
    {
        m_camera = camera;
        m_cameraStart = camera.getWorldTransform();

        GlobalProperties& properties = getServiceProvider().get<GlobalProperties>();
        const float warmup = eastl::clamp(properties.getValue<float>("sample/automation/warmupSeconds").value_or(5.f), 1.f, 120.f);
        const float settle = eastl::clamp(properties.getValue<float>("sample/automation/settleSeconds").value_or(0.f), 0.f, 30.f);
        m_measurementStart = warmup + settle;
        m_measureSeconds = eastl::clamp(properties.getValue<float>("sample/automation/measureSeconds").value_or(15.f), 1.f, 300.f);
        m_cameraMotion = properties.getValue<bool>("sample/automation/cameraMotion").value_or(true);
    }

    void JungleAutomation::updateComponent(float)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_phase == 0)
        {
            m_start = now;
        }
        const float elapsed = std::chrono::duration<float>(now - m_start).count();
        const uint32_t phase = elapsed < m_measurementStart ? 1u : elapsed < m_measurementStart + m_measureSeconds ? 2u : 3u;
        if (phase != m_phase)
        {
            m_phase = phase;
            getServiceProvider().get<GlobalProperties>().setValue("sample/automation/phase", phase).ignore();
            NAU_LOG_INFO("AUTORUN: phase={}", phase);
        }
        if (phase == 3)
        {
            getApplication().stop();
            return;
        }
        if (m_cameraMotion)
        {
            if (scene::SceneObject* camera = m_camera.get())
            {
                const float t = phase == 2 ? elapsed - m_measurementStart : elapsed;
                const math::vec3 offset(0.8f * std::sin(t * 0.6f), 0.15f * std::sin(t * 0.4f), 0.3f * std::sin(t * 0.3f));
                camera->setTranslation(m_cameraStart.getTranslation() + offset);
                camera->setRotation(m_cameraStart.getRotation() * math::quat::rotationY(0.12f * std::sin(t * 0.6f)));
            }
        }
    }
}
