// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "nau/samples/sample_app_delegate.h"

#include <EASTL/algorithm.h>

#include "./sample_components/asset_reloader.h"
#include "./sample_components/scene_reloader.h"
#include "./sample_components/window_maker.h"
#include "./texture_import_settings.h"
#include "nau/app/application_delegate.h"
#include "nau/app/global_properties.h"
#include "nau/graphics/core_graphics.h"
#include "nau/gui/imguiInput.h"
#include "nau/input.h"
#include "nau/platform/windows/app/window_message_handler.h"
#include "nau/render/render_window.h"
#include "nau/samples/components/camera_control.h"
#include "nau/samples/components/rotator.h"
#include "nau/scene/internal/scene_listener.h"
#include "nau/scene/internal/scene_manager_internal.h"
#include "nau/utils/functor.h"

namespace nau::sample
{
    class AmbientOcclusionSampleInput final : public IWindowMessageHandler,
                                              public IRttiObject
    {
        NAU_RTTI_CLASS(nau::sample::AmbientOcclusionSampleInput, IWindowMessageHandler, IRttiObject)

    public:
        AmbientOcclusionSampleInput() :
            m_mode(eastl::min(getServiceProvider().get<GlobalProperties>().getValue<uint32_t>("render/ssao/mode").value_or(1), 2u))
        {
        }

        bool handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM) override
        {
            if (message != WM_KEYUP || wParam != VK_F7)
            {
                return false;
            }
            auto* graphics = getServiceProvider().find<ICoreGraphics>();
            auto window = graphics ? graphics->getDefaultRenderWindow().lock() : nullptr;
            if (!window || window->getHwnd() != hwnd)
            {
                return false;
            }
            m_mode = (m_mode + 1u) % 3u;
            window->setAmbientOcclusionMode(static_cast<render::AmbientOcclusionMode>(m_mode));
            return true;
        }

    private:
        uint32_t m_mode;
    };

    class DemoSceneListener final : public scene::ISceneListener
    {
    public:
        DemoSceneListener() = default;

    private:
        void onSceneBegin() override
        {
        }

        void onSceneEnd() override
        {
        }

        void onAfterActivatingObjects(eastl::span<const scene::SceneObject*> objects) override
        {
        }

        void onBeforeDeletingObjects(eastl::span<const scene::SceneObject*> objects) override
        {
        }

        void onAfterActivatingComponents(eastl::span<const scene::Component*> components) override
        {
        }

        void onBeforeDeletingComponents(eastl::span<const scene::Component*> components) override
        {
        }

        void onComponentsChange(eastl::span<const scene::Component*> components) override
        {
        }
    };

    class MySampleDelegate final : public SampleAppDelegate
    {
    public:
        MySampleDelegate(Functor<async::Task<>()>&& startup) :
            SampleAppDelegate("sceneBase"),
            m_startup(std::move(startup))
        {
        }

    private:
        Result<> initializeServices() override
        {
            if (getServiceProvider().get<GlobalProperties>().getValue<bool>("render/ssao/enabled").value_or(false))
            {
                getServiceProvider().addService<AmbientOcclusionSampleInput>();
            }
            getServiceProvider().addService<TextureImportSettingsProvider>();
            getServiceProvider().addClass<nau::sample::CameraControl>();
            getServiceProvider().addClass<nau::sample::MyRotator>();
            getServiceProvider().addClass<nau::sample::AssetReloader>();
            getServiceProvider().addClass<nau::sample::SceneReloader>();
            getServiceProvider().addClass<nau::sample::WindowMaker>();

            return ResultSuccess;
        }

        async::Task<> startupApplication() override
        {
            // The overview camera shares input with ImGui outside its panels.
            imgui_use_hybrid_input_mode(getServiceProvider().get<GlobalProperties>().getValue<bool>("sample/camera/overview").value_or(false));
            return m_startup();
        }

        Functor<async::Task<>()> m_startup;

        scene::SceneListenerRegistration m_sceneListenerReg;
        DemoSceneListener m_sceneListener;
    };

    ApplicationDelegate::Ptr createSampleAppDelegate(Functor<async::Task<>()>&& startup)
    {
        return eastl::make_unique<MySampleDelegate>(std::move(startup));
    }

}  // namespace nau::sample
