// ao_hbao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ao_algorithm.h"

#include "nau/app/global_properties.h"
#include "nau/service/service_provider.h"
namespace nau::render::ao
{
    class Hbao final : public Algorithm
    {
    public:
        async::Task<> loadAssets() override
        {
            GlobalProperties& properties = getServiceProvider().get<GlobalProperties>();
            const eastl::string path = properties.getValue<eastl::string>("render/ssao/hbaoMaterial").value_or("");
            MaterialAssetRef material{path};
            m_material = co_await material.getAssetViewTyped<MaterialAssetView>();
            if (!m_material)
            {
                co_yield NauMakeError("Failed to load hbao material");
            }
            m_material->enableAutoSetTextures(false);
            struct ShaderBinding
            {
                const char* setting;
                ShaderAssetView::Ptr* destination;
            };
            const ShaderBinding bindings[] = {
                {"render/ssao/hbaoPixelShader", &m_pixel},
                {"render/ssao/hbaoComputeShader", &m_compute}
            };
            for (const auto& [setting, destination] : bindings)
            {
                const eastl::string shaderPath = properties.getValue<eastl::string>(setting).value_or("");
                if (!shaderPath.empty())
                {
                    ShaderAssetRef shader{shaderPath};
                    const auto loaded = co_await shader.getAssetViewTyped<ShaderAssetView>().doTry();
                    if (loaded)
                    {
                        *destination = *loaded;
                    }
                }
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
