// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include <EASTL/vector.h>

#include "nau/3d/dag_drv3d.h"
#include "nau/assets/asset_view.h"
#include "nau/rtti/rtti_impl.h"

namespace nau
{
    class NAU_GRAPHICSASSETS_EXPORT TextureAssetView : public IAssetView
    {
        NAU_CLASS_(nau::TextureAssetView, IAssetView)
    public:
        static async::Task<nau::Ptr<TextureAssetView>> createFromAssetAccessor(nau::Ptr<> accessor);

        inline BaseTexture* getTexture()
        {
            return m_Texture;
        }

        /** One mip level kept on the CPU, in the format of the texture. */
        struct CpuMip
        {
            eastl::vector<uint8_t> data;
            uint32_t rowPitch = 0;
            uint32_t rowCount = 0;
        };

        /**
         * The small mip levels of a texture with alpha, kept on the CPU for code that needs the alpha outside of the driver
         * (masks of shadow casters on the second GPU). Starts with the first level not larger than MAX_CPU_MIP_SIZE.
         */
        static constexpr uint32_t MAX_CPU_MIP_SIZE = 512;

        const eastl::vector<CpuMip>& getCpuMips() const
        {
            return m_cpuMips;
        }

        uint32_t getCpuMipsWidth() const
        {
            return m_cpuMipsWidth;
        }

        uint32_t getCpuMipsHeight() const
        {
            return m_cpuMipsHeight;
        }

        /** DXGI_FORMAT of the CPU mips, 0 if there are none. */
        uint32_t getCpuMipsDxgiFormat() const
        {
            return m_cpuMipsDxgiFormat;
        }

        using Ptr = nau::Ptr<TextureAssetView>;
    private:
        BaseTexture* m_Texture;
        eastl::vector<CpuMip> m_cpuMips;
        uint32_t m_cpuMipsWidth = 0;
        uint32_t m_cpuMipsHeight = 0;
        uint32_t m_cpuMipsDxgiFormat = 0;
    };
}  // namespace nau
