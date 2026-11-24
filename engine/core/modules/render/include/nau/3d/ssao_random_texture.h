// ssao_random_texture.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#include <cstdint>

namespace d3d::mgpu
{
    constexpr uint32_t SSAO_RANDOM_TEXTURE_SIZE = 256;

    /**
     * @brief Generates the same reproducible RGBA8 random-vector texture on both GPUs.
     * @param index The row-major texel index.
     * @return Independent random RGB bytes in [0,255], with opaque alpha.
     */
    inline uint32_t ssaoRandomTexel(uint32_t index)
    {
        const auto hash = [](uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            return value ^ (value >> 16);
        };
        return (hash(index * 3u + 17u) & 255u) |
               ((hash(index * 3u + 18u) & 255u) << 8) |
               ((hash(index * 3u + 19u) & 255u) << 16) | 0xff000000u;
    }
}  // namespace d3d::mgpu
