// Copyright 2026 N-GINN LLC. All rights reserved.

#include "scene_loaders.h"
#include "sample_components/jungle_load.h"

#include <charconv>

#include "nau/app/global_properties.h"
#include "nau/io/file_system.h"
#include "nau/samples/components/camera_control.h"
#include "nau/scene/components/directional_light_component.h"
#include "nau/scene/components/environment_component.h"
#include "nau/serialization/json.h"

namespace nau::sample
{
    async::Task<> startupJungleRuins()
    {
        io::IFileSystem& fileSystem = getServiceProvider().get<io::IFileSystem>();
        io::IFile::Ptr manifestFile = fileSystem.openFile("/content/scenes/jungle_ruins/manifest.json", io::AccessMode::Read, io::OpenFileMode::OpenExisting);
        if (!manifestFile)
        {
            co_yield NauMakeError("JungleRuins is not prepared. Run samples/sceneBase/prepare_jungle_ruins.py with the extracted map directory.");
        }
        io::IStreamReader::Ptr stream = manifestFile->createStream();
        Result<RuntimeValue::Ptr> json = serialization::jsonParse(*stream);
        if (!json)
        {
            co_yield json.getError();
        }
        RuntimeReadonlyDictionary& manifest = (*json)->as<RuntimeReadonlyDictionary&>();
        Result<eastl::vector<eastl::string>> materials = runtimeValueCast<eastl::vector<eastl::string>>(manifest.getValue("materials"));
        if (!materials || materials->empty())
        {
            co_yield NauMakeError("JungleRuins material manifest is invalid.");
        }

        AssetRef<> asset{"file:/content/scenes/jungle_ruins/jungle_ruins.gltf"};
        SceneAsset::Ptr sceneAsset = co_await asset.getAssetViewTyped<SceneAsset>();
        if (!sceneAsset)
        {
            co_yield NauMakeError("Could not load the prepared JungleRuins glTF scene.");
        }
        scene::ISceneFactory& factory = getServiceProvider().get<scene::ISceneFactory>();
        scene::ISceneManager& manager = getServiceProvider().get<scene::ISceneManager>();
        scene::IScene::Ptr scene = factory.createSceneFromAsset(*sceneAsset);
        scene->setName("JungleRuins");
        constexpr eastl::string_view PREFIX = "JungleRuins_m";
        uint32_t meshCount = 0;
        for (scene::SceneObject* object : scene->getRoot().getChildObjects(true))
        {
            scene::StaticMeshComponent* mesh = object->findFirstComponent<scene::StaticMeshComponent>();
            if (!mesh)
            {
                continue;
            }
            const eastl::string_view name = object->getName();
            const size_t separator = name.find('_', PREFIX.size());
            uint32_t materialIndex = 0;
            if (!name.starts_with(PREFIX) || separator == eastl::string_view::npos)
            {
                co_yield NauMakeError("Unexpected mesh name in JungleRuins: {}", name);
            }
            const std::from_chars_result parsed = std::from_chars(name.data() + PREFIX.size(), name.data() + separator, materialIndex);
            if (parsed.ec != std::errc{} || parsed.ptr != name.data() + separator || materialIndex >= materials->size())
            {
                co_yield NauMakeError("Invalid JungleRuins material index: {}", name);
            }
            mesh->setMaterial(MaterialAssetRef{(*materials)[materialIndex]});
            ++meshCount;
        }

        GlobalProperties& properties = getServiceProvider().get<GlobalProperties>();
        scene::SceneObject& cameraObject = scene->getRoot().attachChild(factory.createSceneObject<scene::CameraComponent>());
        cameraObject.setName("Camera.JungleRuins");
        scene::CameraComponent& camera = cameraObject.getRootComponent<scene::CameraComponent>();
        camera.setFov(properties.getValue<float>("sample/camera/fov").value_or(50.0f));
        camera.setClipNearPlane(0.1f);
        camera.setClipFarPlane(250.0f);
        cameraObject.setTranslation({properties.getValue<float>("sample/camera/x").value_or(0.0f),
                                     properties.getValue<float>("sample/camera/y").value_or(1.385f),
                                     properties.getValue<float>("sample/camera/z").value_or(0.0f)});
        cameraObject.setRotation(math::quat::rotationY(properties.getValue<float>("sample/camera/yaw").value_or(0.675966f)) *
                                 math::quat::rotationX(properties.getValue<float>("sample/camera/pitch").value_or(-0.011868f)));
        CameraControl& control = cameraObject.addComponent<CameraControl>();
        control.setStepFactor(5.0f);

        scene::DirectionalLightComponent& light = scene->getRoot().addComponent<scene::DirectionalLightComponent>();
        light.setIntensity(2.0f);
        light.setCastShadows(true);
        light.setShadowMapSize(2048);
        light.setShadowCascadeCount(4);
        light.setRotation(math::quat::rotationZ(math::degToRad(-50.0f)));
        scene->getRoot().addComponent<JungleLoad>().initialize(cameraObject, light);
        scene::EnvironmentComponent& environment = scene->getRoot().addComponent<scene::EnvironmentComponent>();
        environment.setIntensity(0.5f);
        environment.setTextureAsset(TextureAssetRef{"file:/content/textures/environment/SunnyHills_2k.hdr"});

        co_await manager.activateScene(std::move(scene));
        control.setCamControlKind(CamControlKind::UseSceneObject);
        NAU_LOG_INFO("JungleRuins: {} mesh instances, {} materials, meters, fixed camera; alpha-masked vegetation supported", meshCount, materials->size());
    }
}  // namespace nau::sample
