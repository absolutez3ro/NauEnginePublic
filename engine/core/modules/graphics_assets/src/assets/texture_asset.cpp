// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include "../../include/graphics_assets/texture_asset.h"

#include <algorithm>

#include "nau/assets/texture_asset_accessor.h"

#define LOAD_TEXTURE_ASYNC

namespace nau
{
    namespace
    {
        // DXGI_FORMAT of a format that can carry an alpha mask, 0 for the other ones.
        inline uint32_t dxgiFormatWithAlpha(TinyImageFormat tinyFormat)
        {
            switch (tinyFormat)
            {
                case TinyImageFormat_R8G8B8A8_UNORM:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case TinyImageFormat_B8G8R8A8_UNORM:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case TinyImageFormat_DXBC1_RGBA_UNORM:
                    return DXGI_FORMAT_BC1_UNORM;
                case TinyImageFormat_DXBC3_UNORM:
                    return DXGI_FORMAT_BC3_UNORM;
                case TinyImageFormat_DXBC7_UNORM:
                    return DXGI_FORMAT_BC7_UNORM;
                default:
                    return 0;
            }
        }

        inline uint32_t getDagorFormat(TinyImageFormat tinyFormat)
        {
            if (tinyFormat == TinyImageFormat_B8G8R8A8_UNORM)
            {
                return TEXFMT_DEFAULT;
            }
            else  if(tinyFormat == TinyImageFormat_R8G8B8A8_UNORM)
            {
                return TEXFMT_R8G8B8A8;
            }
            else if(tinyFormat == TinyImageFormat_R10G10B10A2_UNORM)
            {
                return TEXFMT_A2R10G10B10;
            }
            else if(tinyFormat == TinyImageFormat_R16G16B16A16_SFLOAT)
            {
                return TEXFMT_A16B16G16R16F;
            }
            else if(tinyFormat == TinyImageFormat_R16G16B16_SFLOAT)
            {
                return TEXFMT_A16B16G16R16F;
            }
            else if(tinyFormat == TinyImageFormat_R32G32B32A32_SFLOAT)
            {
                return TEXFMT_A32B32G32R32F;
            }
            else if(tinyFormat == TinyImageFormat_DXBC1_RGB_UNORM || tinyFormat == TinyImageFormat_DXBC1_RGBA_UNORM)
            {
                return TEXFMT_DXT1;
            }
            else if(tinyFormat == TinyImageFormat_DXBC3_UNORM)
            {
                return TEXFMT_DXT5;
            }
            else if(tinyFormat == TinyImageFormat_DXBC6H_SFLOAT)
            {
                return TEXFMT_BC6H;
            }
            else if(tinyFormat == TinyImageFormat_DXBC7_UNORM)
            {
                return TEXFMT_BC7;
            }

            NAU_FAILURE("Unknown Texture tiny Format: ({})", static_cast<std::underlying_type_t<TinyImageFormat>>(tinyFormat));

            return DXGI_FORMAT_UNKNOWN;
        }
    }  // namespace

    async::Task<nau::Ptr<TextureAssetView>> TextureAssetView::createFromAssetAccessor(nau::Ptr<> accessor)
    {
        using namespace nau::async;

        NAU_ASSERT(accessor);

#ifdef LOAD_TEXTURE_ASYNC
        // ASYNC_SWITCH_EXECUTOR should not be used here:
        // it is necessary to change the execution thread in any case, even if the execution is already taking place in the pool.
        co_await async::Executor::getDefault();
#endif

        auto& textureAccessor = accessor->as<ITextureAssetAccessor&>();
        auto textureAssetView = rtti::createInstance<TextureAssetView>();
        const auto& imageDesc = textureAccessor.getDescription();

        BaseTexture* tex = nullptr;

        const uint32_t           dagorFormat     = getDagorFormat(imageDesc.format);
        const TextureFormatDesc& dagorFormatDesc = get_tex_format_desc(dagorFormat);

        tex = d3d::create_tex(nullptr, imageDesc.width, imageDesc.height, dagorFormat, imageDesc.numMipmaps);

        // Materials sample their textures with the sampler of the texture itself (a material binds no sampler of its own
        // to the slot, see MasterMaterialAssetView::bindPipeline), so the anisotropy is set here: surfaces at a grazing
        // angle, the ground first of all, stay sharp.
        if (tex != nullptr && imageDesc.numMipmaps > 1)
        {
            tex->setAnisotropy(16);
        }


        for(int mipLevel = 0; mipLevel < imageDesc.numMipmaps; ++mipLevel)
        {
            TextureInfo info;
            int t = tex->getinfo(info, mipLevel);

            void* texDataPtr = nullptr;
            int stride;
            tex->lockimg(&texDataPtr, stride, mipLevel, TEXLOCK_WRITE);

            eastl::vector<DestTextureData> dstData(1);
            DestTextureData& data = dstData[0];

            data.outputBuffer = texDataPtr;
            data.rowsCount    = std::max(1, info.h / dagorFormatDesc.elementHeight);
            data.rowPitch     = stride;
            data.rowBytesSize = 0; // will use format's default row bytes size
            data.slicePitch   = 0;

            textureAccessor.copyTextureData(mipLevel, 1, dstData);

            const uint32_t keptDxgiFormat = dxgiFormatWithAlpha(imageDesc.format);
            if (keptDxgiFormat != 0 && static_cast<uint32_t>(std::max(info.w, info.h)) <= TextureAssetView::MAX_CPU_MIP_SIZE)
            {
                if (textureAssetView->m_cpuMips.empty())
                {
                    textureAssetView->m_cpuMipsWidth = info.w;
                    textureAssetView->m_cpuMipsHeight = info.h;
                    textureAssetView->m_cpuMipsDxgiFormat = keptDxgiFormat;
                }

                CpuMip& mip = textureAssetView->m_cpuMips.emplace_back();
                mip.rowPitch = stride;
                mip.rowCount = data.rowsCount;
                mip.data.resize(static_cast<size_t>(mip.rowPitch) * mip.rowCount);

                data.outputBuffer = mip.data.data();
                textureAccessor.copyTextureData(mipLevel, 1, dstData);
            }

            tex->unlockimg();
        }

        textureAssetView->m_Texture = tex;

        co_return textureAssetView;
    }

}  // namespace nau

#ifdef LOAD_TEXTURE_ASYNC
    #undef LOAD_TEXTURE_ASYNC
#endif
