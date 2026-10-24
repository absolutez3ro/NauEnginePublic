// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "graphics_scene.h"
#include "nau/app/main_loop/game_system.h"
#include "nau/async/work_queue.h"
#include "nau/graphics/core_graphics.h"
#include "nau/memory/frame_allocator.h"
#include "nau/render/dag_postFxRenderer.h"
#include "nau/render/deferredRenderer.h"
#include "nau/render/environmentRenderer.h"
#include "nau/rtti/rtti_impl.h"
#include "nau/scene/scene_processor.h"
#include "render/daBfg/bfg.h"
#include "nau/scene/world.h"
#include "render_window/render_window_impl.h"

namespace nau
{
    /**
     */
    class GraphicsImpl final : public ICoreGraphics,
                               public IGameSceneUpdate,
                               public scene::IComponentsAsyncActivator,
                               public IServiceInitialization,
                               public IServiceShutdown
    {
        NAU_RTTI_CLASS(GraphicsImpl,
                       ICoreGraphics,
                       IGameSceneUpdate,
                       scene::IComponentsAsyncActivator,
                       IServiceInitialization,
                       IServiceShutdown)

        NAU_CLASS_ATTRIBUTES(
            CLASS_ATTRIBUTE(PreferredExecutionMode, ExecutionMode::Concurrent),
            CLASS_ATTRIBUTE(SceneAccessMode, SceneAccess::ReadOnly))


    public:
        using AsyncAction = Functor<async::Task<>()>;

        GraphicsImpl();
        ~GraphicsImpl() override;

        async::Task<bool> renderFrame() override;

        async::Task<> activateComponentsAsync(Uid worldUid, eastl::span<const scene::Component*> components, async::Task<> barrier) override;
        async::Task<> deactivateComponentsAsync(Uid worldUid, eastl::span<const scene::DeactivatedComponentData> components) override;

        async::Task<bool> update(std::chrono::milliseconds dt) override;

        eastl::optional<std::chrono::milliseconds> getFixedUpdateTimeStep() override
        {
            return eastl::nullopt;
        }

        void syncSceneState() override;

        async::Task<> preInitService() override;
        async::Task<> initService() override;
        async::Task<> shutdownService() override;

        async::Executor::Ptr getPreRenderExecutor() override;
        void addPreRenderJob(AsyncAction action);

        math::mat4 getProjMatrix() override;

        async::Task<> requestViewportResize(int32_t newWidth, int32_t newHeight, void* hwnd) override;
        
        async::Task<> registerWindow(void* hwnd) override;

        WeakPtr<nau::render::IRenderWindow> getDefaultRenderWindow() override;
        void getRenderWindows(eastl::vector<WeakPtr<nau::render::IRenderWindow>>& windows) override;
        
        void setObjectHighlight(nau::Uid uid, bool flag) override;

        async::Task<> closeWindow(void* hwnd) override;

        async::Task<WeakPtr<nau::render::IRenderWindow>> createRenderWindow(void* hwnd) override;

    private:

        void resizeViewport(int32_t width, int32_t height, SWAPID swapchainID = 0);
        void createDefaultTexture();

        void renderMainScene();



        async::Task<> executeRenderJobs();

        void stopGraphics();

        BaseTexture* m_defaultTex;

        nau::FrameAllocator m_frameAllocator;

        nau::Uid m_defaultWorld = nau::NullUid;
        eastl::map<nau::Uid, eastl::shared_ptr<GraphicsScene>> m_worldToGraphicScene;
        Ptr<nau::render::RenderWindowImpl> m_defaultRenderWindow;
        eastl::map<SWAPID, Ptr<nau::render::RenderWindowImpl>> m_renderWindows;
        uint32_t m_renderWindowsIds = 0;

        eastl::map<void*, SWAPID> m_hwndToSwapChain = {};

        WorkQueue::Ptr m_preRenderWorkQueue = WorkQueue::create();
        std::mutex m_preRenderJobsMutex;
        eastl::vector<AsyncAction> m_preRenderJobs;

        std::atomic<bool> m_isDisposed{false};
        async::TaskSource<> m_renderStopedSignal;

        bool m_isInitialized = false;
        bool m_serializeRenderJobs = false;
    };

}  // namespace nau
