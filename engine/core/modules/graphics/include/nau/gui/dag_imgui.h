// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#pragma once

#include <EASTL/functional.h>

namespace nau
{
    class DataBlock;
}

struct ImDrawData;

enum class ImGuiState
{
  OFF,
  ACTIVE,
  OVERLAY,
  _COUNT, // For iteration purposes only, do not use!
};

using OnStateChangeHandlerFunc = eastl::function<void(ImGuiState, ImGuiState)>;

bool init_on_demand();
//void imgui_set_override_blk(const nau::DataBlock &imgui_blk); // call this before init_on_demand() called
void imgui_enable_imgui_submenu(bool enabled);
void imgui_shutdown();
NAU_GRAPHICS_EXPORT ImGuiState imgui_get_state();
bool imgui_want_capture_mouse();
void imgui_request_state_change(ImGuiState new_state);
void imgui_register_on_state_change_handler(OnStateChangeHandlerFunc func);
NAU_GRAPHICS_EXPORT void imgui_update();
void imgui_endframe();
void imgui_render();

NAU_GRAPHICS_EXPORT void imgui_cache_render_data();
NAU_GRAPHICS_EXPORT void imgui_copy_render_data();
void imgui_render_copied_data();
// The frame imgui_render_copied_data() would draw: the snapshot the main thread handed to the render thread
// (imgui_copy_render_data). The render thread may use it until the next imgui_copy_render_data(); nullptr before the
// first one. Used by the Mgpu UI layer, where GPU1 draws this snapshot instead (mgpu/bridge/mgpu_ui_layer.h).
NAU_GRAPHICS_EXPORT ImDrawData* imgui_get_copied_data();
//nau::DataBlock *imgui_get_blk();
void imgui_save_blk();
void imgui_window_set_visible(const char *group, const char *name, const bool visible);
bool imgui_window_is_visible(const char *group, const char *name);
void imgui_perform_registered();
void imgui_cascade_windows();

typedef eastl::function<void(void)> ImGuiFuncPtr;

struct ImGuiFunctionQueue
{
  ImGuiFunctionQueue *next = nullptr; // single-linked list
  ImGuiFuncPtr function = nullptr;
  const char *group = nullptr;
  const char *name = nullptr;
  const char *hotkey = nullptr;
  int priority = 0; // lower the number, earlier it will be in the list
  int flags = 0;
  bool opened = false;
  static ImGuiFunctionQueue *windowHead;
  static ImGuiFunctionQueue *functionHead;
  ImGuiFunctionQueue(const char *group_, const char *name_, const char *hotkey_, int priority_, int flags_, ImGuiFuncPtr func,
    bool is_window);
};

#define DAG_IGQ_CC0(a, b) a##b
#define DAG_IGQ_CC1(a, b) DAG_IGQ_CC0(a, b)
#define REGISTER_IMGUI_WINDOW(group, name, func) \
  static ImGuiFunctionQueue DAG_IGQ_CC1(AutoImGuiWindow, __LINE__)(group, name, nullptr, 100, 0, func, true)
#define REGISTER_IMGUI_WINDOW_EX(group, name, hotkey, priority, flags, func) \
  static ImGuiFunctionQueue DAG_IGQ_CC1(AutoImGuiWindow, __LINE__)(group, name, hotkey, priority, flags, func, true)
#define REGISTER_IMGUI_FUNCTION(group, name, func) \
  static ImGuiFunctionQueue DAG_IGQ_CC1(AutoImGuiFunction, __LINE__)(group, name, nullptr, 100, 0, func, false)
#define REGISTER_IMGUI_FUNCTION_EX(group, name, hotkey, priority, func) \
  static ImGuiFunctionQueue DAG_IGQ_CC1(AutoImGuiFunction, __LINE__)(group, name, hotkey, priority, 0, func, false)
