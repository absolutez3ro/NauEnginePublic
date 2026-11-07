// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "render/omniShadows.h"

#include "nau/diag/logging.h"
#include "nau/shaders/dag_overrideStateId.h"
#include "render_pipeline/render_scene.h"
#include "render_pipeline/render_view.h"

namespace nau::render
{
    using namespace nau::math;

    OmniShadows::OmniShadows()
    {
        d3d::SamplerInfo samplerInfo;
        samplerInfo.isCompare = true;
        samplerInfo.mip_map_mode = d3d::MipMapMode::Point;
        samplerInfo.filter_mag_mode = d3d::FilterMode::Linear;
        samplerInfo.filter_min_mode = d3d::FilterMode::Linear;
        samplerInfo.address_mode_u = d3d::AddressMode::Clamp;
        samplerInfo.address_mode_v = d3d::AddressMode::Clamp;
        samplerInfo.address_mode_w = d3d::AddressMode::Clamp;
        m_sampler = d3d::create_sampler(samplerInfo);

        m_constantBuffer = d3d::create_cb(sizeof(ShaderConstants), SBCF_DYNAMIC, u8"omni shadow buffer");

        createAtlas();

        shaders::OverrideState state;
        state.set(shaders::OverrideState::Z_BIAS);
        state.set(shaders::OverrideState::Z_FUNC);
        // The z-prepass material culls the other side than the main pass (z_prepass.nmat_json), so the atlas would hold the far
        // surface of every caster: receivers next to a contact with the caster (a mesh sunk into the floor) then read as lit.
        // The faces of a point light are drawn with the cull flipped: the atlas holds the surface nearest to the light.
        state.set(shaders::OverrideState::FLIP_CULL);
        state.zFunc = CMPF_LESSEQUAL;
        state.zBias = omni_shadow::RASTER_Z_BIAS;
        state.slopeZBias = omni_shadow::RASTER_SLOPE_Z_BIAS;
        m_depthBiasOverride.reset(shaders::overrides::create(state));

        // The shader reads the buffer even if no light casts shadows this frame.
        const ShaderConstants empty = {};
        if (m_constantBuffer != nullptr)
        {
            m_constantBuffer->updateDataWithLock(0, sizeof(empty), &empty, VBLOCK_DISCARD);
        }
    }

    OmniShadows::~OmniShadows()
    {
        m_atlas.close();
        if (m_constantBuffer != nullptr)
        {
            m_constantBuffer->destroy();
            m_constantBuffer = nullptr;
        }
    }

    void OmniShadows::createAtlas()
    {
        m_atlas = dag::create_tex(NULL, omni_shadow::ATLAS_WIDTH, omni_shadow::ATLAS_HEIGHT,
                                  TEXFMT_DEPTH16 | TEXCF_RTARGET | TEXCF_TC_COMPATIBLE, 1, "omniShadowAtlas");
        d3d_err(m_atlas.get());
        m_atlas->texfilter(TEXFILTER_COMPARE);
        m_atlas->texaddr(TEXADDR_CLAMP);

        // Readable from the start: the point light pass samples the atlas even before the first face is drawn.
        d3d::resource_barrier({m_atlas.get(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        NAU_LOG_DEBUG("omni shadow atlas created: {}x{}", omni_shadow::ATLAS_WIDTH, omni_shadow::ATLAS_HEIGHT);
    }

    BaseTexture* OmniShadows::getAtlas() const
    {
        return m_atlas.get();
    }

    OmniShadows::AtlasInfo OmniShadows::getAtlasInfo() const
    {
        AtlasInfo info;
        info.width = static_cast<int>(omni_shadow::ATLAS_WIDTH);
        info.height = static_cast<int>(omni_shadow::ATLAS_HEIGHT);
        info.format = TEXFMT_DEPTH16;
        return info;
    }

    Sbuffer* OmniShadows::getConstantBuffer() const
    {
        return m_constantBuffer;
    }

    d3d::SamplerHandle OmniShadows::getSampler() const
    {
        return m_sampler;
    }

    OmniShadows::Snapshot OmniShadows::makeSnapshot(const eastl::vector<OmniShadowCaster>& casters) const
    {
        Snapshot snapshot;
        snapshot.count = static_cast<uint32_t>(std::min<size_t>(casters.size(), omni_shadow::MAX_LIGHTS));
        snapshot.constants.header = Vector4(static_cast<float>(snapshot.count), 0, 0, 0);

        const float width = static_cast<float>(omni_shadow::ATLAS_WIDTH);
        const float height = static_cast<float>(omni_shadow::ATLAS_HEIGHT);
        const float faceSize = static_cast<float>(omni_shadow::FACE_SIZE);

        for (uint32_t slot = 0; slot < snapshot.count; ++slot)
        {
            const OmniShadowCaster& caster = casters[slot];
            ShaderConstants::Slot& out = snapshot.constants.slots[slot];
            snapshot.lightIds[slot] = caster.lightId;

            for (uint32_t face = 0; face < omni_shadow::FACES; ++face)
            {
                out.faceViewProj[face] = caster.faceViewProj[face];

                uint32_t x = 0;
                uint32_t y = 0;
                omni_shadow::faceOrigin(slot, face, x, y);
                // uv = ndc.xy * (0.5 * face / atlas, -0.5 * face / atlas) + centre of the face
                out.faceRect[face] = Vector4(0.5f * faceSize / width, -0.5f * faceSize / height,
                                             (static_cast<float>(x) + 0.5f * faceSize) / width,
                                             (static_cast<float>(y) + 0.5f * faceSize) / height);
            }
            out.posRadius = Vector4(caster.position.getX(), caster.position.getY(), caster.position.getZ(), caster.radius);
            out.params = Vector4(omni_shadow::SHADER_BIAS_METERS, omni_shadow::SHADER_SLOPE_BIAS_METERS, omni_shadow::NEAR_PLANE,
                                 std::max(caster.radius, omni_shadow::NEAR_PLANE * 2.0f));
        }
        return snapshot;
    }

    void OmniShadows::uploadConstants(const Snapshot& snapshot)
    {
        if (m_constantBuffer != nullptr)
        {
            m_constantBuffer->updateDataWithLock(0, sizeof(snapshot.constants), &snapshot.constants, VBLOCK_DISCARD);
        }
    }

    void OmniShadows::renderFaces(const eastl::vector<OmniShadowCaster>& casters, RenderScene& scene)
    {
        if (!m_atlas || casters.empty())
        {
            return;
        }

        d3d::set_render_target();
        d3d::set_render_target(nullptr, 0);
        d3d::set_depth(m_atlas.get(), DepthAccess::RW);
        d3d::clearview(CLEAR_ZBUFFER, 0, 1.f, 0);

        shaders::OverrideStateId currentState = shaders::overrides::get_current();
        if (currentState)
        {
            shaders::overrides::reset();
        }

        MaterialAssetView::Ptr zPrepass = scene.getZPrepassMaterial();
        if (zPrepass)
        {
            for (auto& view : scene.getViews())
            {
                if (!view->containsTag(RenderScene::Tags::shadowOmniTag))
                {
                    continue;
                }

                const uint32_t index = static_cast<uint32_t>(reinterpret_cast<intptr_t>(view->getUserData()));
                const uint32_t slot = index / omni_shadow::FACES;
                const uint32_t face = index % omni_shadow::FACES;
                if (slot >= casters.size() || slot >= omni_shadow::MAX_LIGHTS)
                {
                    continue;
                }

                uint32_t x = 0;
                uint32_t y = 0;
                uint32_t size = 0;
                getFaceViewport(slot, face, x, y, size);
                d3d::setview(static_cast<int>(x), static_cast<int>(y), static_cast<int>(size), static_cast<int>(size), 0.0f, 1.0f);

                shaders::overrides::set(m_depthBiasOverride);
                view->renderZPrepass(casters[slot].faceViewProj[face], zPrepass.get());
                shaders::overrides::reset();
            }
        }

        if (currentState)
        {
            shaders::overrides::set(currentState);
        }

        d3d::set_depth(nullptr, DepthAccess::RW);
        markReadable();
    }

    void OmniShadows::markReadable()
    {
        if (m_atlas)
        {
            d3d::resource_barrier({m_atlas.get(), RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        }
    }

    void OmniShadows::getFaceViewport(const uint32_t slot, const uint32_t face, uint32_t& x, uint32_t& y, uint32_t& size)
    {
        omni_shadow::faceOrigin(slot, face, x, y);
        size = omni_shadow::FACE_SIZE;
    }

    void OmniShadows::getDepthBias(float& constBias, float& slopeBias)
    {
        constBias = omni_shadow::RASTER_Z_BIAS;
        slopeBias = omni_shadow::RASTER_SLOPE_Z_BIAS;
    }
}  // namespace nau::render
