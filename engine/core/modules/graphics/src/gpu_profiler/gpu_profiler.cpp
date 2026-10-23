#include "gpu_profiler.h"

#include <EASTL/algorithm.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/app/global_properties.h"
#include "nau/diag/logging.h"
#include "nau/service/service_provider.h"

namespace nau::gpu_profiler
{
    namespace
    {
        // A source finalizes timestamp queries only after the frame fence has been passed, i.e. a few frames later.
        // Cap the number of frames we keep waiting on; when the cap is hit the new frame is simply not measured.
        constexpr size_t MaxPendingFrames = 8;
        // A transfer half whose pair has not arrived after this many frames is dropped.
        constexpr uint64_t MaxTransferWaitFrames = 4 * MaxPendingFrames;

        const char* transferSuffix(TransferHalf half)
        {
            return half == TransferHalf::Source ? "/src" : "/dst";
        }

        const char* getEnv(const char* name)
        {
            const char* v = std::getenv(name);
            return (v && *v) ? v : nullptr;
        }

        // Quotes a CSV field when needed.
        eastl::string csvEscape(const eastl::string& s)
        {
            if (s.find_first_of(",\"\n") == eastl::string::npos)
            {
                return s;
            }
            eastl::string r = "\"";
            for (char c : s)
            {
                if (c == '"')
                {
                    r += '"';
                }
                r += c;
            }
            r += '"';
            return r;
        }

        // Timestamps of GPU 0, taken on the graphics queue through the engine driver.
        class DriverTimestampSource final : public ITimestampSource
        {
        public:
            uint64_t getFrequency() override
            {
                uint64_t freq = 0;
                if (!d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &freq, nullptr, nullptr))
                {
                    return 0;
                }
                return freq;
            }

            void issue(void*& query) override
            {
                d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &query, nullptr, nullptr);
            }

            bool tryGetValue(void* query, uint64_t& value) override
            {
                return query && d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query, &value, nullptr) != 0;
            }

            void release(void* query) override
            {
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query, nullptr, nullptr);
            }
        };

        DriverTimestampSource g_driverSource;

        GpuMode readGpuMode()
        {
            if (auto* props = getServiceProvider().find<GlobalProperties>())
            {
                if (auto multiGpu = props->getValue<bool>("dx12/multiGpu"))
                {
                    return *multiGpu ? GpuMode::Multi : GpuMode::Single;
                }
            }
            return GpuMode::Single;
        }
    }  // namespace

    const char* toString(GpuMode mode)
    {
        return mode == GpuMode::Multi ? "2gpu" : "1gpu";
    }

    GpuProfiler& GpuProfiler::instance()
    {
        static GpuProfiler profiler;
        return profiler;
    }

    void GpuProfiler::init()
    {
        if (m_initialized)
        {
            return;
        }

        Config config;
        if (const char* v = getEnv("NAU_GPU_PROFILER"))
        {
            config.enabled = std::atoi(v) != 0;
        }
        if (const char* v = getEnv("NAU_GPU_PROFILER_CSV"))
        {
            config.csvPath = v;
        }
        if (const char* v = getEnv("NAU_GPU_PROFILER_LOG_EVERY"))
        {
            config.logEveryNFrames = static_cast<uint32_t>(eastl::max(0, std::atoi(v)));
        }
        if (config.enabled)
        {
            config.mode = readGpuMode();
        }

        init(config);

        if (m_config.enabled && !registerGpu(PrimaryGpu, &g_driverSource))
        {
            NAU_LOG_WARNING("GPU profiler: driver does not provide timestamp frequency, profiler disabled");
            shutdown();
            // Do not retry every frame.
            m_initialized = true;
        }
    }

    void GpuProfiler::init(const Config& config)
    {
        if (m_initialized)
        {
            return;
        }
        m_initialized = true;
        m_config = config;

        if (!m_config.enabled)
        {
            return;
        }

        m_csv.open(m_config.csvPath.c_str(), std::ios::out | std::ios::trunc);
        if (!m_csv)
        {
            NAU_LOG_WARNING("GPU profiler: cannot open '{}' for writing, CSV output disabled", m_config.csvPath.c_str());
        }
        else
        {
            m_csv << "frame,pass,gpu,ms,mode\n";
        }

        NAU_LOG_INFO("GPU profiler enabled: mode {}, CSV '{}', log every {} frames", toString(m_config.mode),
                     m_config.csvPath.c_str(), m_config.logEveryNFrames);
    }

    void GpuProfiler::shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        for (GpuIndex gpu = 0; gpu < MaxGpus; ++gpu)
        {
            unregisterGpu(gpu);
        }

        if (m_csv.is_open())
        {
            m_csv.flush();
            m_csv.close();
        }

        m_transfers.clear();
        m_frameIndex = 0;
        m_initialized = false;
        m_config = {};
    }

    bool GpuProfiler::registerGpu(GpuIndex gpu, ITimestampSource* source)
    {
        if (!m_config.enabled)
        {
            return false;
        }
        if (gpu >= MaxGpus || !source)
        {
            NAU_LOG_WARNING("GPU profiler: cannot register GPU {}", gpu);
            return false;
        }
        if (m_gpus[gpu].source)
        {
            NAU_LOG_WARNING("GPU profiler: GPU {} is already registered", gpu);
            return false;
        }

        const uint64_t freq = source->getFrequency();
        if (freq == 0)
        {
            NAU_LOG_WARNING("GPU profiler: GPU {} does not provide timestamp frequency, it is not measured", gpu);
            return false;
        }

        GpuState& state = m_gpus[gpu];
        state = {};
        state.source = source;
        state.frequency = freq;

        NAU_LOG_INFO("GPU profiler: GPU {} timestamp frequency {} Hz", gpu, freq);
        return true;
    }

    void GpuProfiler::unregisterGpu(GpuIndex gpu)
    {
        if (!hasGpu(gpu))
        {
            return;
        }

        // Give the GPU a chance to finalize what is left; whatever stays unfinished is dropped.
        collectFinished(gpu);

        GpuState& state = m_gpus[gpu];
        for (void* q : state.allQueries)
        {
            state.source->release(q);
        }
        state = {};
    }

    bool GpuProfiler::hasGpu(GpuIndex gpu) const
    {
        return gpu < MaxGpus && m_gpus[gpu].source != nullptr;
    }

    void* GpuProfiler::acquireQuery(GpuState& state)
    {
        if (!state.freeQueries.empty())
        {
            void* q = state.freeQueries.back();
            state.freeQueries.pop_back();
            return q;
        }
        return nullptr;  // created lazily by the source on first issue
    }

    void GpuProfiler::issueTimestamp(GpuState& state, void*& query)
    {
        const bool isNew = (query == nullptr);
        state.source->issue(query);
        if (isNew && query)
        {
            state.allQueries.push_back(query);
        }
    }

    void GpuProfiler::beginFrame()
    {
        if (!m_config.enabled)
        {
            return;
        }

        ++m_frameIndex;
        for (GpuIndex gpu = 0; gpu < MaxGpus; ++gpu)
        {
            if (!hasGpu(gpu))
            {
                continue;
            }
            collectFinished(gpu);

            GpuState& state = m_gpus[gpu];
            state.current = {};
            state.current.frame = m_frameIndex;
            state.openScopes.clear();
            state.frameActive = state.pending.size() < MaxPendingFrames;

            Calibration& cal = state.current.calibration;
            cal.valid = state.frameActive && state.source->getClockCalibration(cal.gpuTicks, cal.cpuTicks, cal.cpuFrequency) &&
                        cal.cpuFrequency != 0;
        }
        dropStaleTransfers();

        beginScope("Frame", PrimaryGpu);
    }

    void GpuProfiler::endFrame()
    {
        if (!m_config.enabled)
        {
            return;
        }

        for (GpuIndex gpu = 0; gpu < MaxGpus; ++gpu)
        {
            if (!hasGpu(gpu) || !m_gpus[gpu].frameActive)
            {
                continue;
            }

            // Close any scope that was left open, including the "Frame" scope.
            GpuState& state = m_gpus[gpu];
            while (!state.openScopes.empty())
            {
                endScope(gpu);
            }

            // A GPU that did no measured work this frame has nothing to wait for.
            if (!state.current.scopes.empty())
            {
                state.pending.push_back(eastl::move(state.current));
            }
            state.current = {};
            state.frameActive = false;
        }
    }

    void GpuProfiler::beginScope(const char* name, GpuIndex gpu)
    {
        if (!m_config.enabled || !hasGpu(gpu) || !m_gpus[gpu].frameActive)
        {
            return;
        }

        GpuState& state = m_gpus[gpu];
        // A scope that cannot be measured is still added, so that endScope() closes the right one.
        Scope& scope = state.current.scopes.emplace_back();
        scope.name = name ? name : "<unnamed>";
        if (state.source->canIssue())
        {
            scope.begin = acquireQuery(state);
            scope.end = acquireQuery(state);
            issueTimestamp(state, scope.begin);
        }

        state.openScopes.emplace_back(state.current.scopes.size() - 1);
    }

    void GpuProfiler::endScope(GpuIndex gpu)
    {
        if (!m_config.enabled || !hasGpu(gpu) || !m_gpus[gpu].frameActive || m_gpus[gpu].openScopes.empty())
        {
            return;
        }

        GpuState& state = m_gpus[gpu];
        Scope& scope = state.current.scopes[state.openScopes.back()];
        state.openScopes.pop_back();
        if (scope.begin && state.source->canIssue())
        {
            issueTimestamp(state, scope.end);
        }
        else
        {
            // The end cannot be written (the command stream is gone): the scope is not measured.
            for (void* q : {scope.begin, scope.end})
            {
                if (q)
                {
                    state.freeQueries.push_back(q);
                }
            }
            scope.begin = nullptr;
            scope.end = nullptr;
        }
    }

    void GpuProfiler::beginTransfer(const char* name, GpuIndex srcGpu, GpuIndex dstGpu, TransferHalf half)
    {
        const GpuIndex gpu = half == TransferHalf::Source ? srcGpu : dstGpu;
        if (!m_config.enabled || !hasGpu(gpu) || !m_gpus[gpu].frameActive)
        {
            return;
        }

        TransferInfo info;
        info.name = name ? name : "<unnamed>";
        info.src = srcGpu;
        info.dst = dstGpu;
        info.half = half;

        const eastl::string scopeName = info.name + transferSuffix(half);
        beginScope(scopeName.c_str(), gpu);
        m_gpus[gpu].current.scopes.back().transfer = eastl::move(info);
    }

    void GpuProfiler::collectFinished(GpuIndex gpu)
    {
        GpuState& state = m_gpus[gpu];

        // Frames of one GPU finish in order, so only look at the front of its queue.
        size_t done = 0;
        for (; done < state.pending.size(); ++done)
        {
            const FrameRecord& rec = state.pending[done];
            bool ready = true;
            for (const Scope& s : rec.scopes)
            {
                // A scope whose query could not be created never becomes ready; it is skipped when written.
                uint64_t v = 0;
                if (s.begin && s.end && !(state.source->tryGetValue(s.begin, v) && state.source->tryGetValue(s.end, v)))
                {
                    ready = false;
                    break;
                }
            }
            if (!ready)
            {
                break;
            }

            writeFrame(gpu, rec);
            for (const Scope& s : rec.scopes)
            {
                for (void* q : {s.begin, s.end})
                {
                    if (q)
                    {
                        state.freeQueries.push_back(q);
                    }
                }
            }
        }

        if (done > 0)
        {
            state.pending.erase(state.pending.begin(), state.pending.begin() + done);
        }
    }

    void GpuProfiler::writeFrame(GpuIndex gpu, const FrameRecord& rec)
    {
        GpuState& state = m_gpus[gpu];
        for (const Scope& s : rec.scopes)
        {
            uint64_t b = 0, e = 0;
            if (!s.begin || !s.end || !state.source->tryGetValue(s.begin, b) || !state.source->tryGetValue(s.end, e))
            {
                continue;
            }

            // Both timestamps come from the same GPU, so they are converted with that GPU's frequency.
            const double ms = (e >= b) ? double(e - b) * 1000.0 / double(state.frequency) : 0.0;

            writeRow(rec.frame, s.name, eastl::to_string(gpu), ms);
            accumulate(state, s.name, ms);

            if (s.transfer)
            {
                addTransferHalf(gpu, rec, s, b, e, ms);
            }
        }

        ++state.framesInAccum;
        if (m_config.logEveryNFrames > 0 && state.framesInAccum >= m_config.logEveryNFrames)
        {
            logSummary(gpu);
        }
    }

    void GpuProfiler::writeRow(uint64_t frame, const eastl::string& pass, const eastl::string& gpu, double ms)
    {
        if (m_csv.is_open())
        {
            m_csv << frame << ',' << csvEscape(pass).c_str() << ',' << gpu.c_str() << ',' << std::fixed << std::setprecision(4) << ms << ','
                  << toString(m_config.mode) << '\n';
        }
    }

    void GpuProfiler::accumulate(GpuState& state, const eastl::string& name, double ms)
    {
        auto it = state.accum.find(name);
        if (it == state.accum.end())
        {
            it = state.accum.insert(eastl::make_pair(name, Accum{})).first;
            state.accumOrder.push_back(name);
        }
        it->second.sumMs += ms;
        it->second.maxMs = eastl::max(it->second.maxMs, ms);
        ++it->second.count;
    }

    void GpuProfiler::addTransferHalf(GpuIndex gpu, const FrameRecord& rec, const Scope& scope, uint64_t begin, uint64_t end, double ms)
    {
        const TransferInfo& info = *scope.transfer;
        auto it = eastl::find_if(m_transfers.begin(), m_transfers.end(), [&](const TransferRecord& t)
        {
            return t.frame == rec.frame && t.info.name == info.name && t.info.src == info.src && t.info.dst == info.dst;
        });
        if (it == m_transfers.end())
        {
            TransferRecord record;
            record.frame = rec.frame;
            record.info = info;
            m_transfers.push_back(eastl::move(record));
            it = m_transfers.end() - 1;
        }

        TransferHalfResult& half = it->halves[size_t(info.half)];
        half.ready = true;
        half.begin = begin;
        half.end = end;
        half.ms = ms;
        half.frequency = m_gpus[gpu].frequency;
        half.calibration = rec.calibration;

        if (it->halves[0].ready && it->halves[1].ready)
        {
            writeTransfer(*it);
            m_transfers.erase(it);
        }
    }

    void GpuProfiler::writeTransfer(const TransferRecord& rec)
    {
        const TransferHalfResult& src = rec.halves[size_t(TransferHalf::Source)];
        const TransferHalfResult& dst = rec.halves[size_t(TransferHalf::Destination)];
        const eastl::string gpus = eastl::to_string(rec.info.src) + ">" + eastl::to_string(rec.info.dst);

        // Each half was measured on its own GPU, so the sum is pure GPU work of the transfer.
        const eastl::string sumName = rec.info.name + "/sum";
        const double sumMs = src.ms + dst.ms;
        writeRow(rec.frame, sumName, gpus, sumMs);
        accumulate(m_gpus[rec.info.dst], sumName + " (" + gpus + ")", sumMs);

        // The span needs one time scale: both GPU clocks are moved to the CPU clock with their own calibration,
        // the GPU timestamps themselves are never subtracted across GPUs.
        if (!src.calibration.valid || !dst.calibration.valid)
        {
            return;
        }
        const uint64_t cpuBase = eastl::min(src.calibration.cpuTicks, dst.calibration.cpuTicks);
        auto toCpuSeconds = [cpuBase](uint64_t ticks, uint64_t frequency, const Calibration& cal)
        {
            const double cpu = double(int64_t(cal.cpuTicks - cpuBase)) / double(cal.cpuFrequency);
            const double gpu = double(int64_t(ticks - cal.gpuTicks)) / double(frequency);
            return cpu + gpu;
        };
        const double start = toCpuSeconds(src.begin, src.frequency, src.calibration);
        const double finish = toCpuSeconds(dst.end, dst.frequency, dst.calibration);

        const eastl::string spanName = rec.info.name + "/span";
        const double spanMs = (finish - start) * 1000.0;
        writeRow(rec.frame, spanName, gpus, spanMs);
        accumulate(m_gpus[rec.info.dst], spanName + " (" + gpus + ")", spanMs);
    }

    void GpuProfiler::dropStaleTransfers()
    {
        const auto stale = [this](const TransferRecord& t)
        {
            return t.frame + MaxTransferWaitFrames < m_frameIndex;
        };
        const auto it = eastl::remove_if(m_transfers.begin(), m_transfers.end(), stale);
        if (it != m_transfers.end())
        {
            NAU_LOG_WARNING("GPU profiler: transfer '{}' of frame {} has only one half measured, dropped", it->info.name.c_str(),
                            it->frame);
            m_transfers.erase(it, m_transfers.end());
        }
    }

    void GpuProfiler::logSummary(GpuIndex gpu)
    {
        if (m_csv.is_open())
        {
            m_csv.flush();
        }

        GpuState& state = m_gpus[gpu];
        std::ostringstream out;
        out << "GPU timings (GPU " << gpu << ", mode " << toString(m_config.mode);
        if (gpu == PrimaryGpu && m_config.mode == GpuMode::Multi && !hasGpu(SecondaryGpu))
        {
            // The config asks for two GPUs, but nothing measures the second one: the run is not a real 2-GPU sample.
            out << ", GPU " << SecondaryGpu << " is not measured";
        }
        out << "), average over " << state.framesInAccum << " frames:";
        for (const eastl::string& name : state.accumOrder)
        {
            const Accum& a = state.accum[name];
            if (a.count == 0)
            {
                continue;
            }
            out << "\n  " << std::left << std::setw(32) << name.c_str() << std::right << std::fixed << std::setprecision(3)
                << std::setw(9) << a.sumMs / a.count << " ms (max " << a.maxMs << " ms)";
        }
        NAU_LOG_INFO("{}", out.str());

        state.accum.clear();
        state.accumOrder.clear();
        state.framesInAccum = 0;
    }
}  // namespace nau::gpu_profiler
