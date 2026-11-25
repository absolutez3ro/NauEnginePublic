// ao_ssao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ao_algorithm.h"
#include "nau/app/global_properties.h"
#include "nau/service/service_provider.h"
namespace nau::render::ao
{
    class Ssao final : public Algorithm
    {
    public:
        async::Task<> loadAssets() override
        {
            GlobalProperties& properties = getServiceProvider().get<GlobalProperties>();
            const eastl::string path = properties.getValue<eastl::string>("render/ssao/material").value_or("");
            if (path.empty())
            {
                co_yield NauMakeError("SSAO requires a project material asset path");
            }
            MaterialAssetRef material{path};
            m_material = co_await material.getAssetViewTyped<MaterialAssetView>();
            if (!m_material)
            {
                co_yield NauMakeError("Failed to load ssao material");
            }
            m_material->enableAutoSetTextures(false);
            const eastl::string shaderPath = properties.getValue<eastl::string>("render/ssao/pixelShader").value_or("");
            if (!shaderPath.empty())
            {
                ShaderAssetRef pixel{shaderPath};
                auto loaded = co_await pixel.getAssetViewTyped<ShaderAssetView>().doTry();
                if (loaded)
                {
                    m_pixel = *loaded;
                }
            }
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
