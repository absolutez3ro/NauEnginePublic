// Copyright 2026 N-GINN LLC. All rights reserved.
#include "jungle_load.h"

#include <cmath>
#include <imgui.h>
#include "nau/app/application.h"
#include "nau/app/global_properties.h"
#include "nau/scene/scene_factory.h"
#include "nau/scene/components/static_mesh_component.h"
#include "nau/scene/components/omnilight_component.h"
#include "nau/service/service_provider.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(JungleLoad)

    void JungleLoad::initialize(scene::SceneObject& camera, scene::DirectionalLightComponent& sun)
    {
        m_camera = camera;
        m_sun = sun;
        m_originalShadowSize = sun.getShadowMapSize();
        auto& properties = getServiceProvider().get<GlobalProperties>();
        m_automated = properties.getValue<bool>("sample/automation/enabled").value_or(false);
        m_recordCpuLoad = properties.getValue<bool>("render/measurements/enabled").value_or(false);
        m_cpuLoadEnabled = properties.getValue<bool>("sample/cpuLoad/enabled").value_or(false);
        const float targetMs = properties.getValue<float>("sample/cpuLoad/targetMs").value_or(8.f);
        m_cpuLoadTargetMs = std::isfinite(targetMs) ? eastl::clamp(targetMs, 0.f, 32.f) : 8.f;
        m_cpuAgentCount = eastl::clamp(properties.getValue<uint32_t>("sample/cpuLoad/agents").value_or(4096), 256u, 65536u);
        NAU_LOG_INFO("JungleRuins CPU load: enabled={}, targetMs={}, agents={}", m_cpuLoadEnabled, m_cpuLoadTargetMs, m_cpuAgentCount);
        if (m_automated)
        {
            // Automated scenarios can use only objects, particles and lights. Never extra shadows.
            m_automationMask = properties.getValue<uint32_t>("sample/automation/loadMask").value_or(0) & 7u;
            m_warmupSeconds = eastl::clamp(properties.getValue<float>("sample/automation/warmupSeconds").value_or(5.f), 1.f, 120.f);
            m_measureSeconds = eastl::clamp(properties.getValue<float>("sample/automation/measureSeconds").value_or(15.f), 1.f, 300.f);
            m_cameraMotion = properties.getValue<bool>("sample/automation/cameraMotion").value_or(true);
            m_pulseLoads = properties.getValue<bool>("sample/automation/pulseLoads").value_or(false);
            m_cameraStart = camera.getWorldTransform();
            for (size_t i=0; i<3; ++i) m_enabled[i] = (m_automationMask & (1u << i)) != 0;
            properties.setValue("sample/automation/activeLoadMask", m_automationMask).ignore();
            NAU_LOG_INFO("AUTORUN: initialized, loadMask={}, extraShadows=false", m_automationMask);
        }
    }

    void JungleLoad::clear(size_t category)
    {
        if (auto* group = m_groups[category].get())
        {
            // Assets can still be loading when the checkbox is cleared.
            // The scene manager only accepts destruction after activation completes.
            if (group->getActivationState() != scene::ActivationState::Active) return;
            group->destroy();
        }
        m_groups[category] = nullptr;
        m_items[category].clear();
        if (category == 3)
            if (auto* sun = m_sun.get()) sun->setShadowMapSize(m_originalShadowSize);
    }

    void JungleLoad::spawn(size_t category)
    {
        auto& factory = getServiceProvider().get<scene::ISceneFactory>();
        auto group = factory.createSceneObject();
        group->setName("JungleRuins.Load");
        if (auto* camera = m_camera.get())
        {
            const auto& transform = camera->getWorldTransform();
            const auto forward = rotate(transform.getRotation(), math::vec3(0,0,-1));
            group->setTranslation(transform.getTranslation() + forward * 10.f);
        }
        const uint32_t count = category == 0 ? 128 : category == 1 ? 1024 : category == 2 ? 48 : 256;
        auto& items = m_items[category];
        items.reserve(count);
        for (uint32_t i=0; i<count; ++i)
        {
            const float phase = static_cast<float>(i) * 2.39996323f;
            const float radius = 2.f + static_cast<float>(i % 13) * 0.45f;
            const math::vec3 offset(std::cos(phase)*radius, 1.f+static_cast<float>(i%7)*0.7f, std::sin(phase)*radius);
            scene::SceneObject::Ptr object;
            if (category == 2)
            {
                object = factory.createSceneObject<scene::OmnilightComponent>();
                auto& light = object->getRootComponent<scene::OmnilightComponent>();
                light.setRadius(8.f);
                light.setIntensity(1.2f);
                light.setColor({0.25f+0.75f*std::abs(std::sin(phase)),0.3f,0.25f+0.75f*std::abs(std::cos(phase))});
                light.setDebugDraw(false);
            }
            else
            {
                object = factory.createSceneObject<scene::StaticMeshComponent>();
                auto& mesh = object->getRootComponent<scene::StaticMeshComponent>();
                mesh.setMeshGeometry(StaticMeshAssetRef{category == 0 ? "file:/content/scenes/scene_demo.gltf+[mesh/2]" :
                    category == 1 ? "file:/content/scenes/scene_demo.gltf+[mesh/0]" : "file:/content/scenes/scene_demo.gltf+[mesh/10]"});
                mesh.setMaterial(MaterialAssetRef{"file:/res/materials/embedded/standard_opaque.nmat_json"});
                mesh.setCastShadow(category == 3);
                const float scale = category == 0 ? 0.3f : category == 1 ? 0.02f + static_cast<float>(i%4)*0.01f : 0.5f;
                object->setScale(math::vec3(scale,scale,scale));
            }
            object->setTranslation(offset);
            auto& attached = group->attachChild(std::move(object));
            items.push_back({attached,offset,phase});
        }
        m_groups[category] = getParentObject().attachChild(std::move(group));
        if (category == 3)
            if (auto* sun = m_sun.get()) sun->setShadowMapSize(4096);
        NAU_LOG_INFO("JungleRuins load: category={}, enabled=true, count={}", category, count);
    }

    void JungleLoad::toggle(size_t category)
    {
        m_enabled[category] = !m_enabled[category];
        if (!m_enabled[category]) NAU_LOG_INFO("JungleRuins load: category={}, enabled=false",category);
    }

    void JungleLoad::updateComponent(float dt)
    {
        m_elapsed += dt;
        if (m_automated) updateAutomation();
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x > 1000.f ? 780.f : 260.f,40.f),ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380.f,350.f),ImGuiCond_FirstUseEver);
        if (ImGui::Begin("JungleRuins load"))
        {
            const char* names[] = {"Moving objects (128)","Particles (1024)","Extra lights (48)","Extra shadows (256 casters, 4096)"};
            for (size_t category=0; category<4; ++category)
            {
                ImGui::PushID(static_cast<int>(category));
                bool active = m_enabled[category];
                if (m_automated) ImGui::Text("%s: %s", names[category], active ? "ON" : "OFF");
                else if (ImGui::Checkbox(names[category], &active)) toggle(category);
                ImGui::PopID();
            }
            if (m_automated) ImGui::TextUnformatted("Automated run: extra shadows locked OFF.");
            ImGui::TextWrapped("Independent loads spawn in front of the camera. Disable removes the added objects; shadows restore the original map size.");
            ImGui::Separator();
            if (m_automated)
                ImGui::Text("CPU workload: %s, %.1f ms target, %u agents", m_cpuLoadEnabled ? "ON" : "OFF", m_cpuLoadTargetMs, m_cpuAgentCount);
            else
            {
                int agents = static_cast<int>(m_cpuAgentCount);
                bool changed = ImGui::Checkbox("CPU workload (simulation)", &m_cpuLoadEnabled);
                changed |= ImGui::SliderFloat("CPU target ms", &m_cpuLoadTargetMs, 0.f, 32.f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::SliderInt("CPU agents", &agents, 256, 65536, "%d", ImGuiSliderFlags_AlwaysClamp);
                m_cpuAgentCount = static_cast<uint32_t>(agents);
                if (changed)
                    NAU_LOG_INFO("JungleRuins CPU load: enabled={}, targetMs={}, agents={}", m_cpuLoadEnabled, m_cpuLoadTargetMs, m_cpuAgentCount);
            }
            ImGui::Text("CPU actual: %.2f ms, %u passes", m_cpuLoadActualMs, m_cpuLoadPasses);
            ImGui::Text("Simulation checksum: %.3f", m_cpuLoadChecksum);
            ImGui::TextWrapped("Agents move, bounce and build matrices on the scene update thread. Target 0 runs one pass.");
        }
        ImGui::End();
        updateCpuLoad();
        for (size_t category=0; category<4; ++category)
        {
            if (m_enabled[category] && !m_groups[category]) spawn(category);
            else if (!m_enabled[category] && m_groups[category]) clear(category);
            for (auto& item : m_items[category])
                if (auto* object = item.object.get())
                {
                    auto position = item.offset;
                    if (category == 1)
                        position.setY(std::fmod(m_elapsed*2.f+item.phase,8.f));
                    else
                        position += math::vec3(std::sin(m_elapsed+item.phase)*0.8f,std::sin(m_elapsed*0.7f+item.phase)*0.4f,0.f);
                    object->setTranslation(position);
                    if (category == 0 || category == 3)
                        object->setRotation(math::quat::rotationY(m_elapsed+item.phase));
                }
        }
    }

    void JungleLoad::updateCpuLoad()
    {
        m_cpuLoadActualMs = 0.f;
        m_cpuLoadPasses = 0;
        if (m_cpuLoadEnabled)
        {
            if (m_cpuAgents.size() != m_cpuAgentCount)
            {
                m_cpuAgents.resize(m_cpuAgentCount);
                for (uint32_t index = 0; index < m_cpuAgentCount; ++index)
                {
                    auto& agent = m_cpuAgents[index];
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        agent.position[axis] = static_cast<float>((index * (17u + axis * 12u)) % 1009u) / 1009.f * 30.f - 15.f;
                        agent.velocity[axis] = 0.25f + static_cast<float>((index + axis * 71u) % 127u) / 127.f;
                    }
                    agent.angle = static_cast<float>(index % 1024u) * (6.2831853f / 1024.f);
                }
            }
            const auto started = std::chrono::steady_clock::now();
            do
            {
                float checksum = 0.f;
                for (auto& agent : m_cpuAgents)
                {
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        agent.position[axis] += agent.velocity[axis] * (1.f / 120.f);
                        if (std::abs(agent.position[axis]) > 16.f)
                        {
                            agent.position[axis] = eastl::clamp(agent.position[axis], -16.f, 16.f);
                            agent.velocity[axis] = -agent.velocity[axis];
                        }
                    }
                    agent.angle += 0.01f;
                    if (agent.angle >= 6.2831853f) agent.angle -= 6.2831853f;
                    const float sine = std::sin(agent.angle);
                    const float cosine = std::cos(agent.angle);
                    agent.world = {cosine, 0.f, -sine, 0.f, 0.f, 1.f, 0.f, 0.f,
                                   sine, 0.f, cosine, 0.f, agent.position[0], agent.position[1], agent.position[2], 1.f};
                    checksum += agent.world[0] + agent.world[2] * 0.5f +
                        agent.world[12] * 0.03125f + agent.world[13] * 0.0625f + agent.world[14] * 0.125f;
                }
                m_cpuLoadChecksum = checksum;
                ++m_cpuLoadPasses;
                m_cpuLoadActualMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
            } while (m_cpuLoadActualMs < m_cpuLoadTargetMs);
        }
        if (m_recordCpuLoad)
        {
            auto& properties = getServiceProvider().get<GlobalProperties>();
            properties.setValue("sample/cpuLoad/enabled", m_cpuLoadEnabled).ignore();
            properties.setValue("sample/cpuLoad/targetMs", m_cpuLoadTargetMs).ignore();
            properties.setValue("sample/cpuLoad/actualMs", m_cpuLoadActualMs).ignore();
            properties.setValue("sample/cpuLoad/agents", m_cpuAgentCount).ignore();
            properties.setValue("sample/cpuLoad/passes", m_cpuLoadPasses).ignore();
            properties.setValue("sample/cpuLoad/checksum", m_cpuLoadChecksum).ignore();
        }
    }

    void JungleLoad::updateAutomation()
    {
        m_enabled[3] = false;
        auto& properties = getServiceProvider().get<GlobalProperties>();
        const auto now = std::chrono::steady_clock::now();
        if (!m_automationReady)
        {
            for (size_t i=0; i<3; ++i)
                if (m_enabled[i] && (!m_groups[i] || m_groups[i]->getActivationState() != scene::ActivationState::Active)) return;
            m_automationReady = true;
            m_automationStart = now;
        }
        const float elapsed = std::chrono::duration<float>(now - m_automationStart).count();
        const uint32_t phase = elapsed < m_warmupSeconds ? 1u : elapsed < m_warmupSeconds + m_measureSeconds ? 2u : 3u;
        if (phase != m_automationPhase)
        {
            m_automationPhase = phase;
            properties.setValue("sample/automation/phase", phase).ignore();
            NAU_LOG_INFO("AUTORUN: phase={}, extraShadows=false", phase);
        }
        if (phase == 3)
        {
            getApplication().stop();
            return;
        }
        const float t = phase == 2 ? elapsed - m_warmupSeconds : elapsed;
        m_elapsed = t;
        const uint32_t mask = m_pulseLoads && phase == 2 && (static_cast<uint32_t>(t / 3.f) & 1u) ? 0u : m_automationMask;
        for (size_t i=0; i<3; ++i)
            if (m_enabled[i] != ((mask & (1u << i)) != 0)) toggle(i);
        properties.setValue("sample/automation/activeLoadMask", mask).ignore();
        if (auto* camera = m_camera.get())
        {
            camera->setTranslation(m_cameraStart.getTranslation() + (m_cameraMotion ? math::vec3(0.8f*std::sin(t*0.6f),0.15f*std::sin(t*0.4f),0.3f*std::sin(t*0.3f)) : math::vec3(0,0,0)));
            camera->setRotation(m_cameraStart.getRotation() * math::quat::rotationY(m_cameraMotion ? 0.12f*std::sin(t*0.6f) : 0.f));
        }
    }
}
