// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include <algorithm>

#include <EASTL/array.h>
#include <EASTL/vector.h>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_resPtr.h"
#include "nau/math/math.h"
#include "nau/render/cascadeShadows.h"

namespace nau
{
    class RenderScene;
}

namespace nau::render
{
    namespace omni_shadow
    {
        constexpr uint32_t MAX_LIGHTS = 2;
        constexpr uint32_t FACES = 6;
        constexpr uint32_t FACE_SIZE = 512;
        constexpr uint32_t BLOCK_FACES_X = 3;
        constexpr uint32_t BLOCK_FACES_Y = 2;
        constexpr uint32_t ATLAS_WIDTH = BLOCK_FACES_X * FACE_SIZE;
        constexpr uint32_t ATLAS_HEIGHT = BLOCK_FACES_Y * FACE_SIZE * MAX_LIGHTS;
        constexpr float NEAR_PLANE = 0.1f;

        constexpr float RASTER_Z_BIAS = 0.00005f;
        constexpr float RASTER_SLOPE_Z_BIAS = 1.0f;
        constexpr float SHADER_BIAS_METERS = 0.01f;
        constexpr float SHADER_SLOPE_BIAS_METERS = 0.08f;

        inline math::Matrix4 faceViewProj(const math::Vector3& position, const float radius, const uint32_t face)
        {
            using namespace nau::math;

            static const float directions[FACES][3] = {
                { 1,  0,  0},
                {-1,  0,  0},
                { 0,  1,  0},
                { 0, -1,  0},
                { 0,  0,  1},
                { 0,  0, -1}
            };
            static const float ups[FACES][3] = {
                {0, 1,  0},
                {0, 1,  0},
                {0, 0,  1},
                {0, 0, -1},
                {0, 1,  0},
                {0, 1,  0}
            };

            const float px = position.getX();
            const float py = position.getY();
            const float pz = position.getZ();
            const Matrix4 view = Matrix4::lookAtRH(Point3(px, py, pz),
                                                   Point3(px + directions[face][0], py + directions[face][1], pz + directions[face][2]),
                                                   Vector3(ups[face][0], ups[face][1], ups[face][2]));

            const float farPlane = std::max(radius, NEAR_PLANE * 2.0f);
            Matrix4 projection = Matrix4::identity();
            projection.setElem(0, 0, 1.0f);
            projection.setElem(1, 1, 1.0f);
            projection.setElem(2, 2, farPlane / (NEAR_PLANE - farPlane));
            projection.setElem(2, 3, -1.0f);
            projection.setElem(3, 2, NEAR_PLANE * farPlane / (NEAR_PLANE - farPlane));
            projection.setElem(3, 3, 0.0f);
            return projection * view;
        }

        inline void faceOrigin(const uint32_t slot, const uint32_t face, uint32_t& x, uint32_t& y)
        {
            x = (face % BLOCK_FACES_X) * FACE_SIZE;
            y = (slot * BLOCK_FACES_Y + face / BLOCK_FACES_X) * FACE_SIZE;
        }
    }  // namespace omni_shadow

    struct OmniShadowCaster
    {
        uint32_t lightId = 0;
        nau::math::Vector3 position = {0, 0, 0};
        float radius = 1.0f;
        eastl::array<nau::math::Matrix4, omni_shadow::FACES> faceViewProj;
    };

    class OmniShadows
    {
    public:
        struct ShaderConstants
        {
            struct Slot
            {
                eastl::array<nau::math::Matrix4, omni_shadow::FACES> faceViewProj;
                eastl::array<nau::math::Vector4, omni_shadow::FACES> faceRect;
                nau::math::Vector4 posRadius;
                nau::math::Vector4 params;
            };
            nau::math::Vector4 header;
            eastl::array<Slot, omni_shadow::MAX_LIGHTS> slots;
        };

        struct AtlasInfo
        {
            int width = 0;
            int height = 0;
            uint32_t format = 0;
        };

        struct Snapshot
        {
            ShaderConstants constants = {};
            eastl::array<uint32_t, omni_shadow::MAX_LIGHTS> lightIds = {};
            uint32_t count = 0;
        };

        OmniShadows();
        ~OmniShadows();

        OmniShadows(const OmniShadows&) = delete;
        OmniShadows& operator=(const OmniShadows&) = delete;

        BaseTexture* getAtlas() const;
        AtlasInfo getAtlasInfo() const;
        Sbuffer* getConstantBuffer() const;
        d3d::SamplerHandle getSampler() const;

        Snapshot makeSnapshot(const eastl::vector<OmniShadowCaster>& casters) const;
        void uploadConstants(const Snapshot& snapshot);

        void renderFaces(const eastl::vector<OmniShadowCaster>& casters, RenderScene& scene);

        void markReadable();

        static void getFaceViewport(uint32_t slot, uint32_t face, uint32_t& x, uint32_t& y, uint32_t& size);
        static void getDepthBias(float& constBias, float& slopeBias);

    private:
        void createAtlas();

        ResPtr<BaseTexture> m_atlas;
        Sbuffer* m_constantBuffer = nullptr;
        d3d::SamplerHandle m_sampler = {};
        shaders::UniqueOverrideStateId m_depthBiasOverride;
    };
}  // namespace nau::render
