// ao_xegtao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/ao_xegtao.h"

#include "ao_algorithm.h"
#include "nau/diag/logging.h"
namespace nau::render::ao
{
    class XeGtao final : public Algorithm
    {
    public:
        async::Task<> loadAssets() override;
        ~XeGtao() override;
        void reset() override;
        bool draw(const Frame& frame) override;
        bool configureSecondary(ExecutorDescription& description) const override;
        const char* label() const override
        {
            return "XeGTAO";
        }
        bool reconstructNormals() const override
        {
            return false;
        }
        uint32_t minimumExtent() const override
        {
            return 16;
        }
        bool unorderedOutput() const override
        {
            return true;
        }

    private:
        std::array<ShaderAssetView::Ptr, 5> m_xeShaders;
        std::array<PROGRAM, 5> m_xePrograms = {BAD_PROGRAM, BAD_PROGRAM, BAD_PROGRAM, BAD_PROGRAM, BAD_PROGRAM};
        d3d::SamplerHandle m_xePointClamp = {};
        BaseTexture *m_xeDepth = nullptr, *m_xePing = nullptr, *m_xePong = nullptr, *m_xeEdges = nullptr;
        bool m_xeAttempted = false, m_resultInitialized = false;
    };
    async::Task<> XeGtao::loadAssets()
    {
        d3d::SamplerInfo sampler;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = d3d::AddressMode::Clamp;
        sampler.filter_min_mode = sampler.filter_mag_mode = d3d::FilterMode::Point;
        sampler.mip_map_mode = d3d::MipMapMode::Point;
        m_xePointClamp = d3d::create_sampler(sampler);
        const char* xeShaderPaths[] = {
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_prefilter.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_main.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.csmain]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_denoise.cs.cslast]",
            "file:/res/shaders/cache/shader_cache.nsbc+[xegtao_composite.cs.csmain]"};
        for (size_t index = 0; index < m_xeShaders.size(); ++index)
        {
            ShaderAssetRef shader{xeShaderPaths[index]};
            auto loaded = co_await shader.getAssetViewTyped<ShaderAssetView>().doTry();
            if (loaded)
            {
                m_xeShaders[index] = *loaded;
                m_xePrograms[index] = ShaderAssetView::makeShaderProgram(eastl::span<ShaderAssetView::Ptr>(&m_xeShaders[index], 1));
            }
            else
            {
                NAU_LOG_ERROR("XeGTAO: compute shader unavailable, {}", xeShaderPaths[index]);
            }
        }
    }
    XeGtao::~XeGtao()
    {
        reset();
        for (auto program : m_xePrograms)
        {
            if (program != BAD_PROGRAM)
            {
                d3d::delete_program(program);
            }
        }
    }
    void XeGtao::reset()
    {
        for (auto** texture : {&m_xeDepth, &m_xePing, &m_xePong, &m_xeEdges})
        {
            if (*texture)
            {
                (*texture)->destroy();
            }
            *texture = nullptr;
        }
        m_xeAttempted = m_resultInitialized = false;
    }
    bool XeGtao::configureSecondary(ExecutorDescription& description) const
    {
        description.createAlgorithm = createXeGtaoAlgorithm;
        description.algorithmShaderCount = static_cast<uint32_t>(m_xeShaders.size());
        for (size_t index = 0; index < m_xeShaders.size(); ++index)
        {
            if (!m_xeShaders[index] || m_xeShaders[index]->getShader()->bytecode.empty())
            {
                return false;
            }
            const auto& bytecode = m_xeShaders[index]->getShader()->bytecode;
            description.algorithmShaders[index] = {bytecode.data(), bytecode.size()};
        }
        return true;
    }
    bool XeGtao::draw(const Frame& frame)
    {
        const auto m_secondaryWidth = frame.width;
        const auto m_secondaryHeight = frame.height;
        auto* m_inputDepth = frame.depth;
        auto* m_inputNormals = frame.normals;
        auto* m_result = frame.output;
        const auto& m_constants = frame.constants;
        for (PROGRAM program : m_xePrograms)
        {
            if (program == BAD_PROGRAM)
            {
                return false;
            }
        }
        if (!m_xeDepth)
        {
            if (m_xeAttempted)
            {
                return false;
            }
            m_xeAttempted = true;
            m_xeDepth = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R16F | TEXCF_UNORDERED, 5, u8"xegtao_depth_mips");
            m_xePing = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32UI | TEXCF_UNORDERED, 1, u8"xegtao_ao_ping");
            m_xePong = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R32UI | TEXCF_UNORDERED, 1, u8"xegtao_ao_pong");
            m_xeEdges = d3d::create_tex(nullptr, m_secondaryWidth, m_secondaryHeight, TEXFMT_R8 | TEXCF_UNORDERED, 1, u8"xegtao_edges");
            if (!m_xeDepth || !m_xePing || !m_xePong || !m_xeEdges)
            {
                NAU_LOG_ERROR("XeGTAO: could not allocate GPU 0 working textures");
                for (BaseTexture** texture : {&m_xeDepth, &m_xePing, &m_xePong, &m_xeEdges})
                {
                    if (*texture)
                    {
                        (*texture)->destroy();
                    }
                    *texture = nullptr;
                }
                return false;
            }
            NAU_LOG_INFO("XeGTAO: GPU 0 AO path=compute, quality=high, denoise=2");
        }
        if (!m_xePing || !m_xePong || !m_xeEdges)
        {
            return false;
        }
        if (!m_resultInitialized)
        {
            // Initialize render-target metadata before the first UAV write.
            d3d::set_render_target(m_result, 0);
            d3d::clearview(CLEAR_TARGET, math::E3DCOLOR(255, 255, 255, 255), 0, 0);
            m_resultInitialized = true;
        }
        d3d::set_render_target();
        d3d::set_depth(nullptr, DepthAccess::RW);
        d3d::set_cs_constbuffer_size(sizeof(m_constants) / 16);
        d3d::set_cs_const(0, &m_constants, sizeof(m_constants) / 16);
        d3d::set_sampler(STAGE_CS, 0, m_xePointClamp);
        d3d::resource_barrier({m_inputDepth, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_inputNormals, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_tex(STAGE_CS, 0, m_inputNormals);
        d3d::set_tex(STAGE_CS, 1, m_inputDepth);

        d3d::resource_barrier({m_xeDepth, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[0]);
        for (uint32_t mip = 0; mip < 5; ++mip)
        {
            d3d::set_rwtex(STAGE_CS, mip, m_xeDepth, 0, mip);
        }
        d3d::dispatch((m_secondaryWidth + 15) / 16, (m_secondaryHeight + 15) / 16, 1);
        for (uint32_t slot = 0; slot < 5; ++slot)
        {
            d3d::set_rwtex(STAGE_CS, slot, nullptr, 0, 0);
        }
        d3d::resource_barrier({m_xeDepth, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});

        d3d::resource_barrier({m_xePing, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_xeEdges, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[1]);
        d3d::set_tex(STAGE_CS, 2, m_xeDepth);
        d3d::set_rwtex(STAGE_CS, 0, m_xePing, 0, 0);
        d3d::set_rwtex(STAGE_CS, 1, m_xeEdges, 0, 0);
        d3d::dispatch((m_secondaryWidth + 7) / 8, (m_secondaryHeight + 7) / 8, 1);
        d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
        d3d::set_rwtex(STAGE_CS, 1, nullptr, 0, 0);
        d3d::resource_barrier({m_xeEdges, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_tex(STAGE_CS, 3, m_xeEdges);

        BaseTexture* source = m_xePing;
        BaseTexture* destination = m_xePong;
        for (size_t pass = 2; pass < 4; ++pass)
        {
            d3d::set_tex(STAGE_CS, 2, nullptr);
            d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
            d3d::resource_barrier({destination, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
            d3d::set_program(m_xePrograms[pass]);
            d3d::set_tex(STAGE_CS, 2, source);
            d3d::set_rwtex(STAGE_CS, 0, destination, 0, 0);
            d3d::dispatch((m_secondaryWidth + 15) / 16, (m_secondaryHeight + 7) / 8, 1);
            d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
            eastl::swap(source, destination);
        }
        d3d::resource_barrier({source, RB_RO_SRV | RB_STAGE_COMPUTE, 0, 0});
        d3d::resource_barrier({m_result, RB_RW_UAV | RB_STAGE_COMPUTE, 0, 0});
        d3d::set_program(m_xePrograms[4]);
        d3d::set_tex(STAGE_CS, 2, source);
        d3d::set_rwtex(STAGE_CS, 0, m_result, 0, 0);
        d3d::dispatch((m_secondaryWidth + 7) / 8, (m_secondaryHeight + 7) / 8, 1);
        d3d::set_rwtex(STAGE_CS, 0, nullptr, 0, 0);
        for (uint32_t slot = 0; slot < 4; ++slot)
        {
            d3d::set_tex(STAGE_CS, slot, nullptr);
        }
        d3d::resource_barrier({m_result, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
        return true;
    }

    eastl::unique_ptr<Algorithm> createXeGtao()
    {
        return eastl::make_unique<XeGtao>();
    }
}  // namespace nau::render::ao
