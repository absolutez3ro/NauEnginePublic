// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "nau/async/task.h"
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"

namespace nau::ui
{
    class Canvas;
    class Node;
}  // namespace nau::ui

namespace nau::sample
{
    // A small cocos2d-x panel (nau_gui node): a translucent background, two labels and a sprite in the bottom right
    // corner of the window. It gives the UI layer (ui_layer in graphics/src/render_window/render_graph.cpp) cocos
    // content next to the ImGui windows. NAU_COCOS_UI=0 skips it.
    // Resources: /res/fonts and /res/Images from resources/, /res/ui/shaders (cocos shaders) from ui_res/ next to the
    // executable (CMakeLists.txt).
    async::Task<> setupCocosUiDemo();

    // Follows the window size: nothing in the engine tells cocos that the window was resized, so every frame this
    // component compares the client size with the last one and, when it changed, gives cocos the new size and moves
    // the panel back to the bottom right corner.
    class CocosUiDemo final : public scene::Component,
                              public scene::IComponentUpdate
    {
    public:
        NAU_OBJECT(CocosUiDemo, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

        void attach(ui::Canvas* canvas, ui::Node* panel);

    private:
        void updateComponent(float dt) override;

        ui::Canvas* m_canvas = nullptr; // owned by UiManager
        ui::Node* m_panel = nullptr; // owned by m_canvas
        unsigned m_width = 0;
        unsigned m_height = 0;
    };
}  // namespace nau::sample
