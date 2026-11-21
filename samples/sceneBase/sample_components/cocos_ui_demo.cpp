// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "./cocos_ui_demo.h"

#include <algorithm>
#include <cstdlib>

#include "nau/app/window_manager.h"
#include "nau/diag/logging.h"
#include "nau/scene/scene_factory.h"
#include "nau/scene/scene_manager.h"
#include "nau/service/service_provider.h"
#include "nau/ui.h"
#include "nau/ui/elements/canvas.h"
#include "nau/ui/elements/draw_node.h"
#include "nau/ui/elements/node.h"
#include "nau/ui/elements/sprite.h"
#include "nau/ui/label.h"

namespace nau::sample
{
    NAU_IMPLEMENT_DYNAMIC_OBJECT(CocosUiDemo)

    namespace
    {
        constexpr float PanelWidth = 320.0f;
        constexpr float PanelHeight = 160.0f;
        constexpr float PanelMargin = 20.0f; // from the right and bottom edges of the window
    }  // namespace

    async::Task<> setupCocosUiDemo()
    {
        if (const char* variable = std::getenv("NAU_COCOS_UI"); variable != nullptr && variable[0] == '0' && variable[1] == '\0')
        {
            NAU_LOG_INFO("cocos UI demo: off (NAU_COCOS_UI=0)");
            co_return;
        }

        auto& uiManager = getServiceProvider().get<ui::UiManager>();

        // Same order as samples/uiSampleApp: the canvas is added first, then the engine scene that holds the UI
        // components (animations, CocosUiDemo) is set and activated at the end. The window size is given to cocos by
        // CocosUiDemo on its first update.
        ui::Canvas* canvas = ui::Canvas::create("cocos_demo");
        uiManager.addCanvas(canvas);
        canvas->setRescalePolicy(ui::RescalePolicy::NoRescale);

        auto engineScene = getServiceProvider().get<scene::ISceneFactory>().createEmptyScene();
        engineScene->setName("UI service scene");
        uiManager.setEngineScene(engineScene.getRef());
        uiManager.configureResourcePath();

        // Panel coordinates: cocos starts in the bottom left corner; the children are placed inside the panel.
        ui::Node* panel = ui::Node::create();
        panel->setContentSize({PanelWidth, PanelHeight});

        // Translucent background: drawn with alpha, so the UI layer has semi-transparent pixels as well.
        ui::DrawNode* background = ui::DrawNode::create();
        const math::vec2 corners[4] = {
            math::vec2(0.0f, 0.0f),
            math::vec2(PanelWidth, 0.0f),
            math::vec2(PanelWidth, PanelHeight),
            math::vec2(0.0f, PanelHeight)};
        background->drawPolygon(corners, 4, math::Color4(0.08f, 0.10f, 0.16f, 0.65f), 2.0f,
                                math::Color4(0.40f, 0.70f, 1.00f, 1.0f));
        panel->addChild(background);

        ui::NauLabel* title = ui::NauLabel::create("cocos2d-x UI", "/res/fonts/OpenSans32.fnt");
        title->setPosition(math::vec2(PanelWidth * 0.5f, 125.0f));
        panel->addChild(title);

        ui::NauLabel* subtitle = ui::NauLabel::create("nau_gui node", "/res/fonts/OpenSans24.fnt");
        subtitle->setPosition(math::vec2(PanelWidth * 0.5f, 85.0f));
        panel->addChild(subtitle);

        ui::Sprite* button = ui::Sprite::create("/res/Images/nau_button_active.png");
        button->setPosition(math::vec2(PanelWidth * 0.5f, 40.0f));
        panel->addChild(button);

        canvas->addChild(panel);

        engineScene->getRoot().addComponent<CocosUiDemo>().attach(canvas, panel);

        co_await getServiceProvider().get<scene::ISceneManager>().activateScene(std::move(engineScene));
        NAU_LOG_INFO("cocos UI demo: panel {}x{} in the bottom right corner", PanelWidth, PanelHeight);
    }

    void CocosUiDemo::attach(ui::Canvas* canvas, ui::Node* panel)
    {
        m_canvas = canvas;
        m_panel = panel;
    }

    void CocosUiDemo::updateComponent([[maybe_unused]] float dt)
    {
        if (!m_canvas || !m_panel)
        {
            return;
        }

        const auto [width, height] = getServiceProvider().get<IWindowManager>().getActiveWindow().getClientSize();
        if (width == 0 || height == 0 || (width == m_width && height == m_height))
        {
            return; // minimized, or the size has not changed
        }
        m_width = width;
        m_height = height;

        const math::vec2 size{float(width), float(height)};
        auto& uiManager = getServiceProvider().get<ui::UiManager>();

        // NoRescale with the reference size equal to the window: one cocos unit is one pixel and the canvas covers
        // the whole window (UiManager places it at the bottom left corner).
        m_canvas->setReferenceSize(size);
        uiManager.setReferenceResolution(size.getX(), size.getY());
        uiManager.setScreenSize(size.getX(), size.getY());

        m_panel->setPosition(math::vec2(
            std::max(PanelMargin, size.getX() - PanelWidth - PanelMargin),
            PanelMargin));

        NAU_LOG_INFO("cocos UI demo: window {}x{}, panel moved to the bottom right corner", width, height);
    }
}  // namespace nau::sample
