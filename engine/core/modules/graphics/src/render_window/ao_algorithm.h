// ao_algorithm.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once
#include <EASTL/unique_ptr.h>

#include <array>

#include "graphics_assets/material_asset.h"
#include "nau/3d/ao_executor.h"
#include "nau/render/render_window.h"
namespace nau::render::ao
{
    struct Frame
    {
        uint32_t width, height;
        BaseTexture* normals;
        BaseTexture* depth;
        BaseTexture* output;
        const AoConstants& constants;
    };
    /** @brief Algorithm assets and local passes, independent of mailbox scheduling. */
    class Algorithm
    {
    public:
        virtual ~Algorithm() = default;
        virtual async::Task<> loadAssets() = 0;
        virtual const char* label() const = 0;
        virtual MaterialAssetView* material() const
        {
            return nullptr;
        }
        virtual void reset()
        {
        }
        virtual bool draw(const Frame&)
        {
            return false;
        }
        virtual bool configureSecondary(ExecutorDescription&) const = 0;
        virtual bool reconstructNormals() const
        {
            return true;
        }
        virtual uint32_t minimumExtent() const
        {
            return 1;
        }
        virtual bool unorderedOutput() const
        {
            return false;
        }
        virtual void configureConstants(AoConstants& constants) const
        {
            constants.parameters[0] = 0.5f;
            constants.options[3] = 14.f;
        }
    };
    eastl::unique_ptr<Algorithm> createSsao();
    eastl::unique_ptr<Algorithm> createAlgorithm(AmbientOcclusionAlgorithm algorithm);
}  // namespace nau::render::ao
