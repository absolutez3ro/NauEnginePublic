// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <array>
#include <chrono>
#include <EASTL/vector.h>
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"
#include "nau/scene/components/directional_light_component.h"
#include "nau/scene/scene_object.h"

namespace nau::sample
{
    class JungleLoad final : public scene::Component, public scene::IComponentUpdate
    {
        NAU_OBJECT(JungleLoad, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

    public:
        void initialize(scene::SceneObject& camera, scene::DirectionalLightComponent& sun);

    private:
        void updateComponent(float dt) override;
        void toggle(size_t category);
        void spawn(size_t category);
        void clear(size_t category);
        void updateAutomation();
        void updateCpuLoad();
        struct CpuAgent
        {
            std::array<float, 3> position{};
            std::array<float, 3> velocity{};
            std::array<float, 16> world{};
            float angle = 0.f;
        };
        struct Item
        {
            scene::ObjectWeakRef<scene::SceneObject> object;
            math::vec3 offset;
            float phase;
        };
        scene::ObjectWeakRef<scene::SceneObject> m_camera;
        scene::ObjectWeakRef<scene::DirectionalLightComponent> m_sun;
        std::array<scene::ObjectWeakRef<scene::SceneObject>,4> m_groups;
        std::array<bool,4> m_enabled{};
        std::array<eastl::vector<Item>,4> m_items;
        float m_elapsed = 0.f;
        uint32_t m_originalShadowSize = 2048;
        bool m_automated = false, m_automationReady = false, m_cameraMotion = true, m_pulseLoads = false;
        uint32_t m_automationMask = 0, m_automationPhase = 0;
        float m_warmupSeconds = 5.f, m_measureSeconds = 15.f;
        math::Transform m_cameraStart;
        std::chrono::steady_clock::time_point m_automationStart;
        eastl::vector<CpuAgent> m_cpuAgents;
        bool m_cpuLoadEnabled = false;
        bool m_recordCpuLoad = false;
        uint32_t m_cpuAgentCount = 4096;
        uint32_t m_cpuLoadPasses = 0;
        float m_cpuLoadTargetMs = 8.f;
        float m_cpuLoadActualMs = 0.f;
        float m_cpuLoadChecksum = 0.f;
    };
}
