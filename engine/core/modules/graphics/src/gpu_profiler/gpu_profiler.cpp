#include "gpu_profiler.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/diag/logging.h"

namespace nau::gpu_profiler
{
    namespace
    {
        // The driver finalizes timestamp queries only after the frame fence has been passed, i.e. a few frames later.
        // Cap the number of frames we keep waiting on; when the cap is hit the new frame is simply not measured.
        constexpr size_t MaxPendingFrames = 8;

        const char* getEnv(const char* name)
        {
            const char* v = std::getenv(name);
            return (v && *v) ? v : nullptr;
        }

        // Quotes a CSV field when needed.
        std::string csvEscape(const std::string& s)
        {
            if (s.find_first_of(",\"\n") == std::string::npos)
            {
                return s;
            }
            std::string r = "\"";
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
    } // namespace

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
        m_initialized = true;

        if (const char* v = getEnv("NAU_GPU_PROFILER"))
        {
            m_config.enabled = std::atoi(v) != 0;
        }
        if (const char* v = getEnv("NAU_GPU_PROFILER_CSV"))
        {
            m_config.csvPath = v;
        }
        if (const char* v = getEnv("NAU_GPU_PROFILER_LOG_EVERY"))
        {
            m_config.logEveryNFrames = static_cast<uint32_t>(std::max(0, std::atoi(v)));
        }

        if (!m_config.enabled)
        {
            return;
        }

        uint64_t freq = 0;
        if (!d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &freq, nullptr, nullptr) || freq == 0)
        {
            NAU_LOG_WARNING("GPU profiler: driver does not provide timestamp frequency, profiler disabled");
            m_config.enabled = false;
            return;
        }
        m_frequency = freq;

        m_csv.open(m_config.csvPath, std::ios::out | std::ios::trunc);
        if (!m_csv)
        {
            NAU_LOG_WARNING("GPU profiler: cannot open '{}' for writing, CSV output disabled", m_config.csvPath);
        }
        else
        {
            m_csv << "frame,pass,gpu,ms\n";
        }

        NAU_LOG_INFO("GPU profiler enabled: timestamp frequency {} Hz, CSV '{}', log every {} frames", m_frequency, m_config.csvPath,
                     m_config.logEveryNFrames);
    }

    void GpuProfiler::shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        if (m_config.enabled)
        {
            // Give the GPU a chance to finalize what is left; whatever stays unfinished is dropped.
            collectFinished();
            if (m_csv.is_open())
            {
                m_csv.flush();
                m_csv.close();
            }
            for (void* q : m_allQueries)
            {
                void* tmp = q;
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &tmp, nullptr, nullptr);
            }
        }

        m_allQueries.clear();
        m_freeQueries.clear();
        m_pending.clear();
        m_current = {};
        m_openScopes.clear();
        m_frameActive = false;
        m_initialized = false;
        m_config = {};
    }

    void* GpuProfiler::acquireQuery()
    {
        if (!m_freeQueries.empty())
        {
            void* q = m_freeQueries.back();
            m_freeQueries.pop_back();
            return q;
        }
        return nullptr; // created lazily by the driver on first TIMESTAMPISSUE
    }

    void GpuProfiler::releaseQuery(void* q)
    {
        if (q)
        {
            m_freeQueries.push_back(q);
        }
    }

    void GpuProfiler::issueTimestamp(void*& q)
    {
        const bool isNew = (q == nullptr);
        d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &q, nullptr, nullptr);
        if (isNew && q)
        {
            m_allQueries.push_back(q);
        }
    }

    void GpuProfiler::beginFrame()
    {
        if (!m_config.enabled)
        {
            return;
        }

        collectFinished();

        ++m_frameIndex;
        m_current = {};
        m_current.frame = m_frameIndex;
        m_openScopes.clear();
        m_frameActive = m_pending.size() < MaxPendingFrames;

        if (m_frameActive)
        {
            beginScope("Frame");
        }
    }

    void GpuProfiler::endFrame()
    {
        if (!m_config.enabled || !m_frameActive)
        {
            return;
        }

        // Close any scope that was left open, including the "Frame" scope.
        while (!m_openScopes.empty())
        {
            endScope();
        }

        m_pending.push_back(std::move(m_current));
        m_current = {};
        m_frameActive = false;
    }

    void GpuProfiler::beginScope(const char* name)
    {
        if (!m_config.enabled || !m_frameActive)
        {
            return;
        }

        Scope scope;
        scope.name = name ? name : "<unnamed>";
        scope.begin = acquireQuery();
        scope.end = acquireQuery();
        issueTimestamp(scope.begin);

        m_current.scopes.push_back(std::move(scope));
        m_openScopes.push_back(m_current.scopes.size() - 1);
    }

    void GpuProfiler::endScope()
    {
        if (!m_config.enabled || !m_frameActive || m_openScopes.empty())
        {
            return;
        }

        Scope& scope = m_current.scopes[m_openScopes.back()];
        m_openScopes.pop_back();
        issueTimestamp(scope.end);
    }

    void GpuProfiler::collectFinished()
    {
        // Frames finish in order, so only look at the front of the queue.
        size_t done = 0;
        for (; done < m_pending.size(); ++done)
        {
            FrameRecord& rec = m_pending[done];
            bool ready = true;
            for (const Scope& s : rec.scopes)
            {
                uint64_t v = 0;
                if (!s.begin || !s.end || !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, s.begin, &v, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, s.end, &v, nullptr))
                {
                    ready = false;
                    break;
                }
            }
            if (!ready)
            {
                break;
            }

            writeFrame(rec, m_frequency);
            for (Scope& s : rec.scopes)
            {
                releaseQuery(s.begin);
                releaseQuery(s.end);
            }
        }

        if (done > 0)
        {
            m_pending.erase(m_pending.begin(), m_pending.begin() + done);
        }
    }

    void GpuProfiler::writeFrame(const FrameRecord& rec, uint64_t freq)
    {
        for (const Scope& s : rec.scopes)
        {
            uint64_t b = 0, e = 0;
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, s.begin, &b, nullptr);
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, s.end, &e, nullptr);

            const double ms = (e >= b) ? double(e - b) * 1000.0 / double(freq) : 0.0;

            if (m_csv.is_open())
            {
                m_csv << rec.frame << ',' << csvEscape(s.name) << ',' << m_config.gpuIndex << ',' << std::fixed << std::setprecision(4)
                      << ms << '\n';
            }

            auto [it, inserted] = m_accum.try_emplace(s.name);
            if (inserted)
            {
                m_accumOrder.push_back(s.name);
            }
            it->second.sumMs += ms;
            it->second.maxMs = std::max(it->second.maxMs, ms);
            ++it->second.count;
        }

        ++m_framesInAccum;
        if (m_config.logEveryNFrames > 0 && m_framesInAccum >= m_config.logEveryNFrames)
        {
            logSummary();
        }
    }

    void GpuProfiler::logSummary()
    {
        if (m_csv.is_open())
        {
            m_csv.flush();
        }

        std::ostringstream out;
        out << "GPU timings (GPU " << m_config.gpuIndex << "), average over " << m_framesInAccum << " frames:";
        for (const std::string& name : m_accumOrder)
        {
            const Accum& a = m_accum[name];
            if (a.count == 0)
            {
                continue;
            }
            out << "\n  " << std::left << std::setw(32) << name << std::right << std::fixed << std::setprecision(3) << std::setw(9)
                << a.sumMs / a.count << " ms (max " << a.maxMs << " ms)";
        }
        NAU_LOG_INFO("{}", out.str());

        m_accum.clear();
        m_accumOrder.clear();
        m_framesInAccum = 0;
    }
} // namespace nau::gpu_profiler
