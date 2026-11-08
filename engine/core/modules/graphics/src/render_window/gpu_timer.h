// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include <EASTL/array.h>

#include <cstdint>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_drv3dCmd.h"

namespace nau::render
{
    // GPU time of a stretch of commands on Nau's graphics queue, from the timestamp queries of the driver. The results come
    // a few frames later: a ring of query pairs, each read back once the driver has it. Render thread only.
    class GpuTimer
    {
    public:
        void begin()
        {
            Pair& pair = m_ring[m_next];
            if (pair.pending && !read(pair))
            {
                // The oldest pair is still in flight: this stretch is not measured.
                m_begun = false;
                return;
            }
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &pair.begin, nullptr, nullptr);
            m_begun = true;
        }

        void end()
        {
            if (!m_begun)
            {
                return;
            }
            Pair& pair = m_ring[m_next];
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &pair.end, nullptr, nullptr);
            pair.pending = true;
            m_begun = false;
            m_next = (m_next + 1) % RING;
        }

        // Adds the measurements finished since the last call: milliseconds and their count.
        void collect(double& milliseconds, uint32_t& count)
        {
            for (Pair& pair : m_ring)
            {
                if (pair.pending)
                {
                    read(pair);
                }
            }
            milliseconds += m_milliseconds;
            count += m_count;
            m_milliseconds = 0;
            m_count = 0;
        }

    private:
        static constexpr uint32_t RING = 8;

        struct Pair
        {
            void* begin = nullptr; // queries of the driver, created by its first D3V3D_COMMAND_TIMESTAMPISSUE
            void* end = nullptr;
            bool pending = false;
        };

        bool read(Pair& pair)
        {
            uint64_t begin = 0;
            uint64_t end = 0;
            if (!d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, pair.begin, &begin, nullptr) ||
                !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, pair.end, &end, nullptr))
            {
                return false;
            }
            pair.pending = false;

            if (m_frequency == 0)
            {
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &m_frequency, nullptr, nullptr);
            }
            if (m_frequency != 0 && end >= begin)
            {
                m_milliseconds += static_cast<double>(end - begin) * 1000.0 / static_cast<double>(m_frequency);
                ++m_count;
            }
            return true;
        }

        eastl::array<Pair, RING> m_ring;
        uint32_t m_next = 0;
        bool m_begun = false;
        uint64_t m_frequency = 0;
        double m_milliseconds = 0;
        uint32_t m_count = 0;
    };
}  // namespace nau::render
