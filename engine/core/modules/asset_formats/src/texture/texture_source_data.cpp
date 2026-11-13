// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.
//

#include "texture_source_data.h"

#include "./texture_utils.h"
#include "nau/service/service_provider.h"
#include "texture_asset_container.h"

// Nothings
#define STBI_NO_STDIO
#define STBI_ASSERT(x) NAU_ASSERT(x)
// TODO: replace malloc/realloc/free with our allocator
#define STBI_MALLOC(sz) malloc(sz)
#define STBI_REALLOC(p, newsz) realloc(p, newsz)
#define STBI_FREE(p) free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_ds.h"
#include "stb_image_resize2.h"
#include "tinyimageformat.h"
#include "texture_compressor.h"

namespace nau
{
    class StbLoader
    {
    public:
        StbLoader(io::IStreamReader::Ptr stream, int& width, int& height, int& components, stbi_uc*& data) :
            m_stream(std::move(stream))
        {
            NAU_ASSERT(m_stream);

            m_stream->setPosition(io::OffsetOrigin::End, 0);
            m_streamSize = m_stream->getPosition();
            m_stream->setPosition(io::OffsetOrigin::Begin, 0);

            NAU_ASSERT(m_streamSize < INT32_MAX);

            stbi_io_callbacks cb = {read, skip, eof};

            data = stbi_load_from_callbacks(&cb, this, &width, &height, &components, 0);
            NAU_ASSERT(data);
        }

        StbLoader(io::IStreamReader::Ptr stream, int& width, int& height, int& components, float*& floatData) :
            m_stream(std::move(stream))
        {
            NAU_ASSERT(m_stream);

            m_stream->setPosition(io::OffsetOrigin::End, 0);
            m_streamSize = m_stream->getPosition();
            m_stream->setPosition(io::OffsetOrigin::Begin, 0);

            NAU_ASSERT(m_streamSize < INT32_MAX);

            stbi_io_callbacks cb = {read, skip, eof};

            floatData = stbi_loadf_from_callbacks(&cb, this, &width, &height, &components, 4);
            NAU_ASSERT(floatData);
        }

    private:
        static int read(void* user, char* dat, int size)
        {
            return static_cast<StbLoader*>(user)->read(dat, size);
        }
        static void skip(void* user, int n)
        {
            return static_cast<StbLoader*>(user)->skip(n);
        }
        static int eof(void* user)
        {
            return static_cast<StbLoader*>(user)->eof();
        }

        // stbi_load will call this to get bytes
        int read(char* data, int size)
        {
            return *m_stream->read(reinterpret_cast<std::byte*>(data), size);
        }

        // stbi_load will call this to skip bytes. It will cause addData to
        // add bytes until fifo_in reaches fifo_out.
        void skip(int size)
        {
            m_stream->setPosition(io::OffsetOrigin::Current, size);
        }

        // stbi_load will call this to check for eof.
        // stbi will only call this if the file has an error.
        int eof()
        {
            return m_stream->getPosition() == m_streamSize;
        }

    private:
        io::IStreamReader::Ptr m_stream;
        size_t m_streamSize;
    };

    Result<TextureSourceData> TextureSourceData::loadFromStream(io::IStreamReader::Ptr stream, RuntimeReadonlyDictionary::Ptr importSettings, TinyImageFormat forceFormat)
    {
        ImportSettings settings{};
        if(importSettings)
        {
            runtimeValueApply(settings, importSettings).ignore();
        }
        if (forceFormat != TinyImageFormat_UNDEFINED)
        {
            settings.isCompressed = false;
            settings.generateMipmaps = false;
        }

        int width = 0;
        int height = 0;
        int components = 0;
        unsigned numMipmaps = 1;  // TODO: get num mipmaps in stb loader.
        TinyImageFormat compressedFormat = TinyImageFormat_UNDEFINED; //TODO: get isCompressed in stb loader.
        stbi_uc* data = nullptr;
        float* floatData = nullptr;

        const bool isFloatTexture = forceFormat != TinyImageFormat_UNDEFINED;
        if (isFloatTexture)
        {
            StbLoader{stream, width, height, components, floatData};
        }
        else
        {
            StbLoader{stream, width, height, components, data};
        }

        NAU_ASSERT(data || floatData);

        const auto getFormat = [](TinyImageFormat forceFormat, unsigned numComponent) -> TinyImageFormat
        {
            if (forceFormat != TinyImageFormat_UNDEFINED)
            {
                return forceFormat;
            }
            if(numComponent == 4)
            {
                return TinyImageFormat_R8G8B8A8_UNORM;
            }
            else if(numComponent == 3)
            {
                return TinyImageFormat_R8G8B8_UNORM;
            }
            else if(numComponent == 2)
            {
                return TinyImageFormat_R8G8_UNORM;
            }
            else if(numComponent == 1)
            {
                return TinyImageFormat_R8_UNORM;
            }

            NAU_FAILURE("Unsupported image components count ({})", numComponent);
            return TinyImageFormat_UNDEFINED;
        };

        auto format = getFormat(forceFormat, components);

        if(settings.generateMipmaps && numMipmaps == 1)
        {
            const int numChannels = TinyImageFormat_ChannelCount(format);
            const unsigned targetWidth = TextureUtils::roundToPowOf2(width);
            const unsigned targetHeight = TextureUtils::roundToPowOf2(height);

            numMipmaps = std::max(static_cast<unsigned>(log2(targetWidth)), static_cast<unsigned>(log2(targetHeight))) + 1u;

            if(targetWidth != width || targetHeight != height)
            {
                stbi_uc* const resizedTexture = reinterpret_cast<stbi_uc*>(STBI_MALLOC(targetWidth * targetHeight * numChannels));
                stbir_resize_uint8_linear(
                    data, width, height, numChannels * width, resizedTexture, targetWidth, targetHeight, numChannels * targetWidth, stbir_pixel_layout(numChannels));
                STBI_FREE(data);
                data = resizedTexture;
                width = targetWidth;
                height = targetHeight;
            }
        }
        // FIXME: rgb doesn't work with BC1 compression
        if(format == TinyImageFormat_R8G8B8_UNORM /* && !settings.isCompressed */)
        {
            // uncompressed format does not supports 3 component textures
            constexpr int ForceComponents = 4;

            data = stbi__convert_format(data, components, ForceComponents, width, height);
            format = getFormat(forceFormat,ForceComponents);
        }

        if(settings.isCompressed)
        {
            compressedFormat = TextureCompressor::getOutputTextureFormat(format);
        }

        void* anyData = isFloatTexture ? static_cast<void*>(floatData) : static_cast<void*>(data);
        return TextureSourceData{static_cast<unsigned>(width), static_cast<unsigned>(height), numMipmaps, format, compressedFormat, anyData};
    }

    TextureSourceData::TextureSourceData(unsigned width, unsigned height, unsigned numMipmaps, TinyImageFormat format, TinyImageFormat compressedFormat, void* data) :
        m_width(width),
        m_height(height),
        m_numMipmaps(numMipmaps),
        m_format(format),
        m_compressedFormat(compressedFormat),
        m_data(data)
    {
    }

    TextureSourceData::TextureSourceData(TextureSourceData&& other) :
        m_width(std::exchange(other.m_width, 0)),
        m_height(std::exchange(other.m_height, 0)),
        m_numMipmaps(std::exchange(other.m_numMipmaps, 1)),
        m_format(std::exchange(other.m_format, TinyImageFormat_UNDEFINED)),
        m_compressedFormat(std::exchange(other.m_compressedFormat, TinyImageFormat_UNDEFINED)),
        m_data(std::exchange(other.m_data, nullptr))
    {
    }

    TextureSourceData::~TextureSourceData()
    {
        if(m_data)
        {
            STBI_FREE(m_data);
        }
    }

    TextureSourceData& TextureSourceData::operator=(TextureSourceData&& other)
    {
        NAU_ASSERT(m_data);

        m_width = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_numMipmaps = std::exchange(other.m_numMipmaps, 0);
        m_format = std::exchange(other.m_format, TinyImageFormat_UNDEFINED);
        m_compressedFormat = std::exchange(other.m_compressedFormat, TinyImageFormat_UNDEFINED);
        m_data = std::exchange(other.m_data, nullptr);

        return *this;
    }

    TextureSourceData::operator bool() const
    {
        return m_data != nullptr;
    }

    bool TextureSourceData::isCompressed() const
    {
        return m_compressedFormat != TinyImageFormat_UNDEFINED;
    }

    unsigned TextureSourceData::getWidth() const
    {
        return m_width;
    }
    unsigned TextureSourceData::getHeight() const
    {
        return m_height;
    }
    unsigned TextureSourceData::getNumMipmaps() const
    {
        return m_numMipmaps;
    }
    TinyImageFormat TextureSourceData::getFormat() const
    {
        return isCompressed() ? m_compressedFormat : m_format;
    }

    void TextureSourceData::copyTextureData(size_t mipLevelStart, size_t mipLevelsCount, eastl::span<DestTextureData> destination) const
    {
        struct Mip
        {
            unsigned width;
            unsigned height;
            unsigned char* data;
        };

        NAU_ASSERT(mipLevelsCount >= 0);
        NAU_ASSERT(mipLevelsCount <= m_numMipmaps);

        std::optional<Mip> prevMip;

        const uint32_t channels = TinyImageFormat_ChannelCount(getFormat());
        for(uint32_t i = 0; i < mipLevelsCount; ++i)
        {
            const auto mipLevelIndex = mipLevelStart + i;

            Mip mip;

            if(mipLevelIndex == 0)
            {
                NAU_ASSERT(!prevMip);
                mip.width = m_width;
                mip.height = m_height;
                mip.data = reinterpret_cast<unsigned char*>(m_data);
            }
            else
            {
                if(!prevMip)
                {
                    prevMip = Mip{m_width, m_height, reinterpret_cast<unsigned char*>(m_data)};
                }

                std::tie(mip.width, mip.height) = TextureUtils::getMipSize(getWidth(), getHeight(), mipLevelIndex);
                mip.data = reinterpret_cast<unsigned char*>(STBI_MALLOC(mip.width * mip.height * channels));

                [[maybe_unused]] unsigned char* const mipTexureData = stbir_resize_uint8_linear(
                    prevMip->data, static_cast<int>(prevMip->width), static_cast<int>(prevMip->height), static_cast<int>(channels * prevMip->width),
                    mip.data, static_cast<int>(mip.width), static_cast<int>(mip.height), static_cast<int>(channels * mip.width), stbir_pixel_layout(channels));

                NAU_ASSERT(mipTexureData == mip.data);
            }

            if(m_compressedFormat != TinyImageFormat_UNDEFINED)
            {
                TextureCompressor compressor{m_format};
                unsigned char* compressed_data = compressor.compress(mip.data, mip.width, mip.height);
                NAU_ASSERT(compressed_data);
                TextureUtils::copyImageData(destination[i], mip.width, mip.height, getFormat(), reinterpret_cast<std::byte*>(compressed_data));
                STBI_FREE(compressed_data);
            }
            else
            {
                TextureUtils::copyImageData(destination[i], mip.width, mip.height, getFormat(), reinterpret_cast<std::byte*>(mip.data));
            }

            // NOTE: do not delete original (mip0) data. A generated level is told by its buffer: comparing the sizes kept the
            // levels of a narrow texture, whose one side stays the size of level 0.
            if(prevMip && prevMip->data != reinterpret_cast<unsigned char*>(m_data))
            {
                STBI_FREE(prevMip->data);
            }

            prevMip = mip;
        }

        // The last generated level: it was never freed, and TextureAssetView asks for one level per call, so every level but
        // 0 of every texture leaked (a third of the texture, ~0.4 GB per visit of Jungle Ruins).
        if(prevMip && prevMip->data != reinterpret_cast<unsigned char*>(m_data))
        {
            STBI_FREE(prevMip->data);
        }
    }

    const void* TextureSourceData::getTextureData() const
    {
        return m_data;
    }
}  // namespace nau
