// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "graphics_impl.h"
#include "render_window/render_window_impl.h"
#include "nau/module/module.h"

namespace nau
{
    struct GraphicsModule : public IModule
    {
        nau::string getModuleName() override
        {
            return "GraphicsModule";
        }
        void initialize() override
        {
            NAU_MODULE_EXPORT_CLASS(GraphicsImpl);
            NAU_MODULE_EXPORT_CLASS(nau::render::RenderWindowImpl);
        }
        void deinitialize() override
        {
        }
        void postInit() override
        {
        }
    };
}  // namespace nau

IMPLEMENT_MODULE(nau::GraphicsModule);
