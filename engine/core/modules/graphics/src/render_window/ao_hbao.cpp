// ao_hbao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ao_algorithm.h"
namespace nau::render::ao
{
    class Hbao final : public Algorithm
    {
    public:
        async::Task<> loadAssets() override
        {
            MaterialAssetRef material{"file:/res/materials/hbao.nmat_json"};
            m_material = co_await material.getAssetViewTyped<MaterialAssetView>();
            if (!m_material)
            {
                co_yield NauMakeError("Failed to load hbao material");
            }
            m_material->enableAutoSetTextures(false);
            ShaderAssetRef pixel{"file:/res/shaders/cache/shader_cache.nsbc+[hbao.ps.psmain]"};
            m_pixel = co_await pixel.getAssetViewTyped<ShaderAssetView>();
            ShaderAssetRef compute{"file:/res/shaders/cache/shader_cache.nsbc+[hbao.cs.csmain]"};
            auto loaded = co_await compute.getAssetViewTyped<ShaderAssetView>().doTry();
            if (loaded)
            {
                m_compute = *loaded;
            }
        }
        const char* label() const override
        {
            return "HBAO";
        }
        MaterialAssetView* material() const override
        {
            return m_material.get();
        }
        bool configureSecondary(ExecutorDescription& description) const override
        {
            if (!m_pixel)
            {
                return false;
            }
            const auto& pixel = m_pixel->getShader()->bytecode;
            description.rawBytecode = pixel.data();
            description.rawBytecodeSize = pixel.size();
            if (m_compute)
            {
                const auto& compute = m_compute->getShader()->bytecode;
                description.rawComputeBytecode = compute.data();
                description.rawComputeBytecodeSize = compute.size();
            }
            return !pixel.empty();
        }
        void configureConstants(AoConstants& constants) const override
        {
            constants.parameters[0] = 2.f;
            constants.options[3] = 16.f;
        }

    private:
        MaterialAssetView::Ptr m_material;
        ShaderAssetView::Ptr m_pixel;
        ShaderAssetView::Ptr m_compute;
    };
    eastl::unique_ptr<Algorithm> createHbao()
    {
        return eastl::make_unique<Hbao>();
    }
}  // namespace nau::render::ao
