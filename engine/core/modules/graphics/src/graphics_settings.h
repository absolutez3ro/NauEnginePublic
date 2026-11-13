// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nau/app/global_properties.h"
#include "nau/diag/logging.h"
#include "nau/service/service_provider.h"

namespace nau
{
    /**
     * Rendering switches of the graphics module. Read once, from the "graphics" section of the application properties
     * (the application config, e.g. config/app.json); an environment variable overrides a value, for comparisons and
     * debugging:
     *
     *   graphics/framePipelining              NAU_FRAME_PIPELINING      the CPU records a frame while the GPU draws the
     *                                                                   previous one (frames with pre-render work still wait)
     *   graphics/renderOptimizations          NAU_RENDER_OPTIMIZATIONS  false: the old way of building the render lists,
     *                                                                   kept for comparisons
     *   graphics/shadows/sparseFrames         NAU_SHADOW_SPARSE_FRAMES  a far shadow cascade keeps its shadow map while the
     *                                                                   view stays inside it, and is drawn anew at least every
     *                                                                   that many frames (plus its number); 0: every frame
     *   graphics/shadows/farCascadeLodScale   NAU_SHADOW_LOD_SCALE      the far cascades choose the levels of detail as if
     *                                                                   the casters were that many times farther; 1: off
     *   graphics/shadows/shadowOnlyLods       NAU_SHADOW_ONLY_LODS      the far cascades draw the shadow-only levels of
     *                                                                   detail of the meshes that have them
     */
    struct GraphicsSettings
    {
        bool framePipelining = true;
        bool renderOptimizations = true;
        int shadowSparseFrames = 4;
        float farCascadeLodScale = 2.0f;
        bool shadowOnlyLods = true;
    };

    /**
     * The first far shadow cascade. The far cascades take the coarser levels of detail and are updated sparsely; the near
     * ones are drawn every frame (with Mgpu: by the scene GPU, see nau::mgpu::bridge::Settings::nearCascades).
     */
    constexpr int FAR_SHADOW_CASCADE = 2;

    namespace settings_detail
    {
        inline GlobalProperties* findGlobalProperties()
        {
            return hasServiceProvider() && getServiceProvider().has<GlobalProperties>() ? &getServiceProvider().get<GlobalProperties>()
                                                                                       : nullptr;
        }

        template <typename T>
        void readProperty(GlobalProperties* properties, const char* path, T& value)
        {
            if (properties != nullptr)
            {
                if (const eastl::optional<T> property = properties->getValue<T>(path))
                {
                    value = *property;
                }
            }
        }

        inline void readEnvironment(const char* name, bool& value)
        {
            if (const char* const text = std::getenv(name))
            {
                value = std::strcmp(text, "0") != 0;
            }
        }

        inline void readEnvironment(const char* name, int& value)
        {
            if (const char* const text = std::getenv(name))
            {
                value = std::atoi(text);
            }
        }

        inline void readEnvironment(const char* name, float& value)
        {
            if (const char* const text = std::getenv(name))
            {
                value = static_cast<float>(std::atof(text));
            }
        }
    }  // namespace settings_detail

    inline const GraphicsSettings& getGraphicsSettings()
    {
        static const GraphicsSettings settings = []
        {
            using namespace settings_detail;
            GraphicsSettings result;
            GlobalProperties* const properties = findGlobalProperties();
            readProperty(properties, "graphics/framePipelining", result.framePipelining);
            readProperty(properties, "graphics/renderOptimizations", result.renderOptimizations);
            readProperty(properties, "graphics/shadows/sparseFrames", result.shadowSparseFrames);
            readProperty(properties, "graphics/shadows/farCascadeLodScale", result.farCascadeLodScale);
            readProperty(properties, "graphics/shadows/shadowOnlyLods", result.shadowOnlyLods);

            readEnvironment("NAU_FRAME_PIPELINING", result.framePipelining);
            readEnvironment("NAU_RENDER_OPTIMIZATIONS", result.renderOptimizations);
            readEnvironment("NAU_SHADOW_SPARSE_FRAMES", result.shadowSparseFrames);
            readEnvironment("NAU_SHADOW_LOD_SCALE", result.farCascadeLodScale);
            readEnvironment("NAU_SHADOW_ONLY_LODS", result.shadowOnlyLods);

            result.shadowSparseFrames = std::max(0, result.shadowSparseFrames);
            if (!(result.farCascadeLodScale >= 1.0f && result.farCascadeLodScale <= 16.0f))
            {
                result.farCascadeLodScale = 1.0f;
            }

            NAU_LOG_INFO("graphics settings: frame pipelining {}, render optimizations {}, far shadow cascades drawn anew {}, "
                         "their levels of detail at {}x the distance, shadow-only levels {}",
                         result.framePipelining ? "on" : "off", result.renderOptimizations ? "on" : "off",
                         result.shadowSparseFrames > 0 ? ::fmt::format("when the view leaves them, at least every {} frames", result.shadowSparseFrames)
                                                       : std::string("every frame"),
                         result.farCascadeLodScale, result.shadowOnlyLods ? "on" : "off");
            return result;
        }();
        return settings;
    }
}  // namespace nau
