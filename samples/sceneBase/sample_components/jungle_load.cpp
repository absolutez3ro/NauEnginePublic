// Copyright 2026 N-GINN LLC. All rights reserved.
#include "jungle_load.h"

#include <cmath>
#include <imgui.h>
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
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x > 1000.f ? 780.f : 260.f,40.f),ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350.f,250.f),ImGuiCond_FirstUseEver);
        if (ImGui::Begin("JungleRuins load"))
        {
            const char* names[] = {"Moving objects (128)","Particles (1024)","Extra lights (48)","Extra shadows (256 casters, 4096)"};
            for (size_t category=0; category<4; ++category)
            {
                ImGui::PushID(static_cast<int>(category));
                bool active = m_enabled[category];
                if (ImGui::Checkbox(names[category], &active)) toggle(category);
                ImGui::PopID();
            }
            ImGui::TextWrapped("Independent loads spawn in front of the camera. Disable removes the added objects; shadows restore the original map size.");
        }
        ImGui::End();
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
}
