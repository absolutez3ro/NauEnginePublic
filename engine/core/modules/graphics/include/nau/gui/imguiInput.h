// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#pragma once

#include <EASTL/functional.h>

#include "nau/graphics/graphics_config.h"
#include "nau/math/math.h"

using GlobalInputHandler = eastl::function<bool(/*key_down*/ bool, /*key_idx*/ int, /*key_modif*/ unsigned int)>;

const nau::math::IVector2 &imgui_get_saved_mouse_pos();
bool imgui_handle_special_keys_down(bool ctrl, bool shift, bool alt, int btn_idx, unsigned int key_modif);
bool imgui_handle_special_keys_up(bool ctrl, bool shift, bool alt, int btn_idx, unsigned int key_modif);
void imgui_register_global_input_handler(GlobalInputHandler);

bool imgui_in_hybrid_input_mode();
NAU_GRAPHICS_EXPORT void imgui_use_hybrid_input_mode(bool);
void imgui_switch_state();
void imgui_switch_overlay();
void imgui_set_viewport_offset(int offsetX, int offsetY);
void imgui_draw_mouse_cursor(bool draw_mouse_cursor);
