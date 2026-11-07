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

// Shadow maps of point (omni) lights.
//
// Every shadow-casting point light gets 6 faces (+X, -X, +Y, -Y, +Z, -Z) of a 90 degree perspective projection. The faces of
// all lights are laid out in one depth atlas: a light takes a 3 x 2 block of FACE_SIZE squares, the blocks are stacked
// vertically. Nau draws the faces itself (OmniShadows::renderFaces) or, with the Mgpu path, the second GPU draws them and
// the result is copied into the same atlas. The deferred point light pass finds the face by the largest axis of
// (pixel - light) and compares the depth in the face (point_lights_shader.hlsl, omni_shadow.hlsli).
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

        // Depth bias of the face rendering (override state: constant and slope-scaled, as for the cascades), and the bias of
        // the comparison in the shader, in metres along the face axis (constant + slope part scaled by 1 - N.L).
        constexpr float RASTER_Z_BIAS = 0.00005f;
        constexpr float RASTER_SLOPE_Z_BIAS = 1.0f;
        constexpr float SHADER_BIAS_METERS = 0.01f;
        constexpr float SHADER_SLOPE_BIAS_METERS = 0.08f;

        // Faces in the order of the shader (omniShadowFace in omni_shadow.hlsli): +X, -X, +Y, -Y, +Z, -Z.
        inline math::Matrix4 faceViewProj(const math::Vector3& position, const float radius, const uint32_t face)
        {
            using namespace nau::math;

            static const float directions[FACES][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            static const float ups[FACES][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};

            const float px = position.getX();
            const float py = position.getY();
            const float pz = position.getZ();
            const Matrix4 view = Matrix4::lookAtRH(Point3(px, py, pz),
                                                   Point3(px + directions[face][0], py + directions[face][1], pz + directions[face][2]),
                                                   Vector3(ups[face][0], ups[face][1], ups[face][2]));

            // Right-handed perspective, 90 degrees, depth 0..1 (Direct3D).
            const float n = NEAR_PLANE;
            const float f = std::max(radius, NEAR_PLANE * 2.0f);
            Matrix4 proj = Matrix4::identity();
            proj.setElem(0, 0, 1.0f);
            proj.setElem(1, 1, 1.0f);
            proj.setElem(2, 2, f / (n - f));
            proj.setElem(2, 3, -1.0f);
            proj.setElem(3, 2, n * f / (n - f));
            proj.setElem(3, 3, 0.0f);
            return proj * view;
        }

        // Origin of a face in the atlas, pixels.
        inline void faceOrigin(const uint32_t slot, const uint32_t face, uint32_t& x, uint32_t& y)
        {
            x = (face % BLOCK_FACES_X) * FACE_SIZE;
            y = (slot * BLOCK_FACES_Y + face / BLOCK_FACES_X) * FACE_SIZE;
        }
    }  // namespace omni_shadow

    // A point light that casts shadows this frame. Built by GraphicsScene from the scene components.
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
        // The constant buffer of the point light shader (OmniShadowBuffer in omni_shadow.hlsli).
        struct ShaderConstants
        {
            struct Slot
            {
                eastl::array<nau::math::Matrix4, omni_shadow::FACES> faceViewProj;
                eastl::array<nau::math::Vector4, omni_shadow::FACES> faceRect;  // xy: uv scale of ndc, zw: uv of the face centre
                nau::math::Vector4 posRadius;
                nau::math::Vector4 params;  // x: constant bias, y: slope bias (metres), z: near plane, w: far plane (radius)
            };
            nau::math::Vector4 header;  // x: number of slots
            eastl::array<Slot, omni_shadow::MAX_LIGHTS> slots;
        };

        struct AtlasInfo
        {
            int width = 0;
            int height = 0;
            uint32_t format = 0;
        };

        // Shader constants of one frame, to be bound together with the atlas drawn for that frame.
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

        // Nau draws the faces of all casters itself (one GPU, or the second GPU is not available).
        void renderFaces(const eastl::vector<OmniShadowCaster>& casters, RenderScene& scene);

        // Nothing is drawn: the atlas is left as it is, readable.
        void markReadable();

        // For the second GPU: the face of the atlas as the d3d::setview arguments, and the depth bias of its pipeline.
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
