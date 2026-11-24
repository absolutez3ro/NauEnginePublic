// ao_ssao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ao_algorithm.h"
namespace nau::render::ao
{
    class Ssao final : public Algorithm
    {
    public:
        async::Task<> loadAssets() override
        {
            MaterialAssetRef material{"file:/res/materials/ssao.nmat_json"};
            m_material = co_await material.getAssetViewTyped<MaterialAssetView>();
            if (!m_material)
            {
                co_yield NauMakeError("Failed to load ssao material");
            }
            m_material->enableAutoSetTextures(false);
            ShaderAssetRef pixel{"file:/res/shaders/cache/shader_cache.nsbc+[ssao.ps.psmain]"};
            m_pixel = co_await pixel.getAssetViewTyped<ShaderAssetView>();
        }
        const char* label() const override
        {
            return "SSAO";
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

            return !pixel.empty();
        }

    private:
        MaterialAssetView::Ptr m_material;
        ShaderAssetView::Ptr m_pixel;
    };
    eastl::unique_ptr<Algorithm> createSsao()
    {
        return eastl::make_unique<Ssao>();
    }
}  // namespace nau::render::ao
