// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "nau/3d/dag_drv3d.h"
#include "nau/math/dag_color.h"
#include "nau/scene/nau_object.h"
#include "nau/scene/scene.h"
#include "nau/scene/world.h"


namespace nau::render
{
    using namespace nau::scene;

    NAU_DEFINE_ENUM(
        NauRenderStage,
        uint32_t,
        "nau::render::NauRenderStage",
        GBufferStage = NauFlag(1),
        OutlineStage = NauFlag(2),
        EnvironmentStage = NauFlag(3),
        NauGUIStage = NauFlag(4),
        PostFXStage = NauFlag(5),
        UIDStage = NauFlag(6),
        DebugStage = NauFlag(7))

    NAU_DEFINE_TYPED_FLAG(NauRenderStage)

    enum class AmbientOcclusionMode : uint32_t
    {
        off = 0,
        lighting = 1,
        debug = 2
    };

    struct NAU_ABSTRACT_TYPE IRenderWindow : public virtual IRefCounted
    {
        NAU_INTERFACE(nau::render::IRenderWindow, IRefCounted)

        using Ptr = Ptr<IRenderWindow>;
        using WeakRef = WeakPtr<IRenderWindow>;

        virtual void setWorld(nau::Uid world) = 0;
        virtual nau::Uid getWorld() = 0;

        virtual int32_t getMainCameraIndex() = 0;
        virtual void setMainCameraIndex(int32_t ind) = 0;

        /**
         * @brief Returns the most recently requested SSAO mode. Thread safe.
         * @return The requested mode, independent of whether SSAO was enabled at startup.
         */
        virtual AmbientOcclusionMode getAmbientOcclusionMode() const = 0;
        /**
         * @brief Requests a mode for SSAO enabled at startup, preserving its resources. Thread safe.
         * With render/ssao/enabled=false this has no rendering effect and cannot enable SSAO at runtime.
         * @param mode The requested SSAO mode.
         */
        virtual void setAmbientOcclusionMode(AmbientOcclusionMode mode) = 0;

        virtual async::Task<> requestViewportResize(int32_t width, int32_t height) = 0;
        virtual async::Task<> requestViewportResizeImmediate(int32_t width, int32_t height) = 0;
        virtual void getViewportSize(int32_t& width, int32_t& height) = 0;

        virtual async::Task<> enableRenderStages(::nau::TypedFlag<NauRenderStage> stages) = 0;
        virtual async::Task<> disableRenderStages(::nau::TypedFlag<NauRenderStage> stages) = 0;

        virtual async::Task<nau::Uid> requestUidByCoords(int32_t viewportX, int32_t viewportY) = 0;

        virtual void setOutlineColor(const math::Color4& color) = 0;
        virtual void setOutlineWidth(float newWidth) = 0;

        virtual void setDrawViewportGrid(bool isDrawGrid) = 0;
        virtual bool getDrawViewportGrid() = 0;

        virtual void*  getHwnd() const = 0;
        virtual eastl::string_view getName() const = 0;

        virtual SWAPID getSwapchain() const = 0;
        virtual void initialize(eastl::string_view name, SWAPID swapchain, void* hwnd) = 0;

    };
}  // namespace nau::render
