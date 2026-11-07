// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "nau/animation/components/skeleton_component.h"
#include "nau/math/math.h"
#include "nau/scene/camera/camera_manager.h"
#include "nau/scene/components/scene_component.h"
#include "nau/scene/scene_processor.h"
#include "nau/shaders/shader_defines.h"

#include "graphics_assets/material_asset.h"
#include "graphics_assets/static_mesh_asset.h"
#include "graphics_assets/skinned_mesh_asset.h"
#include "render_pipeline/static_mesh_manager.h"
#include "render_pipeline/skinned_mesh_manager.h"
#include "render_pipeline/billboards_manager.h"
#include "render/lights/clusteredLights.h"
#include "render/omniShadows.h"

#include "graphics_nodes.h"


namespace nau::scene
{
    class CameraComponent;
}


namespace nau
{
    class GraphicsScene
    {
    public:
        GraphicsScene(const GraphicsScene&) = delete;
        GraphicsScene(Uid worldUid);

        async::Task<> initialize();

        async::Task<> activateComponents(eastl::span<const scene::Component*> components, async::Task<> barrier);

        async::Task<> deactivateComponents(eastl::span<const scene::DeactivatedComponentData> components);

        async::Task<> update();

        void renderFrame(bool withGBuffer = false);
        void renderDepth();
        void renderOutlineMask();
        void renderTranslucency();
        void renderLights();
        void renderDebugLights();
        void renderSceneDebug();
        void renderBillboards();

        void syncSceneState();

        CameraNode& getMainCamera();
        bool hasCamera();
        bool hasMainCamera() const;

        bool hasDirectionalLight();
        const eastl::vector<DirectionalLightNode>& getDirectionalLights() const;

        bool hasEnvironmentNode();
        EnvironmentNode& getEnvironmentNode();
        
        void setObjectHighlight(nau::Uid uid, bool flag);

        nau::RenderScene* getRenderScene();

        // Point lights that cast shadows (OmnilightComponent::getCastShadows), up to render::omni_shadow::MAX_LIGHTS. The list is
        // rebuilt by syncSceneState; the copy is safe to take from any thread.
        eastl::vector<render::OmniShadowCaster> getOmniShadowCasters() const;

        // The point light pass gets the shadow data of the atlas it is going to sample: the lights by slot, the constant
        // buffer and the atlas (nullptr atlas: no shadows).
        void setOmniShadowBinding(const eastl::array<uint32_t, render::omni_shadow::MAX_LIGHTS>& lightIds,
                                  uint32_t count,
                                  Sbuffer* constants,
                                  BaseTexture* atlas,
                                  d3d::SamplerHandle sampler);

    private:
        void syncSceneCameras();

        const Uid m_worldUid;

        eastl::vector<StaticMeshNode> m_staticMeshes;
        eastl::vector<BillboardNode> m_billboards;
        eastl::vector<SkinnedMeshNode> m_skinnedMeshes;
        eastl::vector<DirectionalLightNode> m_directionalLights;
        eastl::vector<EnvironmentNode> m_envNodes;
        eastl::vector<LightNode> m_lightNodes;
        eastl::unordered_map<Uid, CameraNode> m_cameras;

        render::ClusteredLights m_lights;

        mutable std::mutex m_omniShadowMutex;
        eastl::vector<render::OmniShadowCaster> m_omniShadowCasters;
        eastl::array<std::atomic<bool>, render::omni_shadow::MAX_LIGHTS> m_omniSlotActive;
        
        nau::Ptr<nau::RenderScene> m_renderScene;

        shaders::RenderStateId fakeId;
        scene::ICameraManager::CamerasSnapshot m_allInGameCameras;
    };
}  // namespace nau
