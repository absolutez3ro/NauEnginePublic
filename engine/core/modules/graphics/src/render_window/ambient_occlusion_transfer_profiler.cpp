// ambient_occlusion_transfer_profiler.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include <EASTL/array.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "ambient_occlusion_profiler.h"
#include "gpu_profiler/d3d12_timestamp_source.h"
#include "nau/diag/logging.h"

namespace nau::render
{
    namespace
    {
        class SsaoTransferProfiler final : public d3d::mgpu::ITransferProfiler
        {
            static constexpr uint32_t MAX_PENDING = 128;

        public:
            SsaoTransferProfiler(const char* name, bool fromPrimary) :
                m_name(name),
                m_src(fromPrimary ? gpu_profiler::PrimaryGpu : gpu_profiler::SecondaryGpu),
                m_dst(fromPrimary ? gpu_profiler::SecondaryGpu : gpu_profiler::PrimaryGpu)
            {
            }
            SsaoTransferProfiler(const SsaoTransferProfiler&) = delete;
            SsaoTransferProfiler(SsaoTransferProfiler&&) = delete;
            SsaoTransferProfiler& operator=(const SsaoTransferProfiler&) = delete;
            SsaoTransferProfiler& operator=(SsaoTransferProfiler&&) = delete;
            ~SsaoTransferProfiler() override = default;

            void attach(bool source, void* device, void* queue) override
            {
                Side& side = m_sides[source ? 0 : 1];
                side.available = side.timestamps.init(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue), MAX_PENDING * 2);
                side.pending.reserve(MAX_PENDING);
                if (side.available)
                {
                    NAU_LOG_INFO("GPU profiler: transfer '{}' {} copy timestamp frequency {} Hz", m_name.c_str(),
                                 source ? "src" : "dst", side.timestamps.getFrequency());
                }
                else
                {
                    NAU_LOG_WARNING("GPU profiler: transfer '{}' {} copy timestamps unavailable", m_name.c_str(), source ? "src" : "dst");
                }
            }

            void beginCopy(bool source, void* list, uint64_t ticket) override
            {
                Side& side = m_sides[source ? 0 : 1];
                collect(m_sides[0], true);
                collect(m_sides[1], false);
                TicketFrame& origin = m_ticketFrames[ticket % m_ticketFrames.size()];
                if (source)
                {
                    origin = {ticket, gpu_profiler::GpuProfiler::instance().frameIndex()};
                }
                if (!side.available || side.pending.size() >= MAX_PENDING || origin.ticket != ticket || origin.frame == 0)
                {
                    return;
                }
                side.current = {};
                side.current.ticket = ticket;
                side.current.timing.frame = origin.frame;
                side.current.timing.frequency = side.timestamps.getFrequency();
                gpu_profiler::TimestampCalibration& calibration = side.current.timing.calibration;
                calibration.valid = side.timestamps.getClockCalibration(calibration.gpuTicks, calibration.cpuTicks, calibration.cpuFrequency);
                side.timestamps.setCommandList(static_cast<ID3D12GraphicsCommandList*>(list));
                side.timestamps.issue(side.current.begin);
            }

            void endCopy(bool source, void* list) override
            {
                Side& side = m_sides[source ? 0 : 1];
                if (side.current.begin)
                {
                    side.timestamps.issue(side.current.end);
                    side.timestamps.resolve(static_cast<ID3D12GraphicsCommandList*>(list));
                    side.timestamps.setCommandList(nullptr);
                }
            }

            void submitted(bool source) override
            {
                Side& side = m_sides[source ? 0 : 1];
                if (side.current.begin)
                {
                    side.timestamps.submitted();
                    side.pending.push_back(side.current);
                    side.current = {};
                }
            }

            void cancel(bool source) override
            {
                Side& side = m_sides[source ? 0 : 1];
                side.timestamps.release(side.current.begin);
                side.timestamps.release(side.current.end);
                side.timestamps.setCommandList(nullptr);
                side.current = {};
            }

            void detach() override
            {
                // The channel calls detach after both COPY queues drain.
                for (uint32_t i = 0; i < m_sides.size(); ++i)
                {
                    Side& side = m_sides[i];
                    collect(side, i == 0);
                    side.timestamps.shutdown();
                    side.pending.clear();
                    side.current = {};
                    side.available = false;
                }
            }

        private:
            struct Measurement
            {
                uint64_t ticket = 0;
                void* begin = nullptr;
                void* end = nullptr;
                gpu_profiler::TransferTiming timing;
            };

            struct Side
            {
                gpu_profiler::D3D12TimestampSource timestamps;
                eastl::vector<Measurement> pending;
                Measurement current;
                bool available = false;
            };

            struct TicketFrame
            {
                uint64_t ticket = 0;
                uint64_t frame = 0;
                gpu_profiler::TransferTiming timings[2];
                bool ready[2] = {};
            };

            void collect(Side& side, bool source)
            {
                size_t done = 0;
                for (Measurement& measurement : side.pending)
                {
                    if (!side.timestamps.tryGetValue(measurement.begin, measurement.timing.begin) ||
                        !side.timestamps.tryGetValue(measurement.end, measurement.timing.end))
                    {
                        break;
                    }
                    TicketFrame& pair = m_ticketFrames[measurement.ticket % m_ticketFrames.size()];
                    if (pair.ticket == measurement.ticket)
                    {
                        const uint32_t index = source ? 0 : 1;
                        pair.timings[index] = measurement.timing;
                        pair.ready[index] = true;
                        // Publish both halves together so a slow destination cannot expire the source measurement.
                        if (pair.ready[0] && pair.ready[1])
                        {
                            recordHalf(true, pair.timings[0]);
                            recordHalf(false, pair.timings[1]);
                            pair.ready[0] = pair.ready[1] = false;
                        }
                        else if (!m_sides[1 - index].available)
                        {
                            recordHalf(source, measurement.timing);
                            pair.ready[index] = false;
                        }
                    }
                    side.timestamps.release(measurement.begin);
                    side.timestamps.release(measurement.end);
                    ++done;
                }
                side.pending.erase(side.pending.begin(), side.pending.begin() + done);
            }

            void recordHalf(bool source, const gpu_profiler::TransferTiming& timing)
            {
                gpu_profiler::GpuProfiler::instance().recordTransferHalf(m_name.c_str(), m_src, m_dst,
                                                                         source ? gpu_profiler::TransferHalf::Source : gpu_profiler::TransferHalf::Destination,
                                                                         timing);
            }

            eastl::string m_name;
            gpu_profiler::GpuIndex m_src;
            gpu_profiler::GpuIndex m_dst;
            eastl::array<Side, 2> m_sides;
            eastl::array<TicketFrame, 64> m_ticketFrames = {};
        };
    }  // namespace

    eastl::shared_ptr<d3d::mgpu::ITransferProfiler> createSsaoTransferProfiler(const char* name, bool fromPrimary)
    {
        return gpu_profiler::GpuProfiler::instance().isEnabled() ? eastl::make_shared<SsaoTransferProfiler>(name, fromPrimary) : nullptr;
    }
}  // namespace nau::render
