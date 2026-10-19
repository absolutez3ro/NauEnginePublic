// GPU timestamp profiler: measures GPU time of render passes (framegraph nodes) and of the whole frame.
// Results are written to a CSV file (frame,pass,gpu,ms) and periodically summarized into the log.
//
// Configuration (environment variables):
//   NAU_GPU_PROFILER=1              enable the profiler (disabled by default)
//   NAU_GPU_PROFILER_CSV=<path>     CSV output path (default: gpu_timings.csv in the working directory)
//   NAU_GPU_PROFILER_LOG_EVERY=<N>  print averaged timings to the log every N frames (default: 120, 0 = never)
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nau::gpu_profiler
{
    struct Config
    {
        bool enabled = false;
        std::string csvPath = "gpu_timings.csv";
        uint32_t logEveryNFrames = 120;
        // Index of the GPU the measurements belong to (column "gpu" in the CSV).
        // Always 0 for now: only a single GPU is measured.
        uint32_t gpuIndex = 0;
    };

    class GpuProfiler
    {
    public:
        static GpuProfiler& instance();

        // Reads the configuration from the environment on first use.
        void init();
        void shutdown();

        bool isEnabled() const { return m_config.enabled; }
        const Config& config() const { return m_config; }

        // Must be called on the render thread while the GPU is owned (between ACQUIRE/RELEASE_OWNERSHIP).
        void beginFrame();
        void endFrame();

        void beginScope(const char* name);
        void endScope();

    private:
        struct Scope
        {
            std::string name;
            void* begin = nullptr;
            void* end = nullptr;
        };

        struct FrameRecord
        {
            uint64_t frame = 0;
            std::vector<Scope> scopes;
        };

        struct Accum
        {
            double sumMs = 0.0;
            double maxMs = 0.0;
            uint32_t count = 0;
        };

        void* acquireQuery();
        void releaseQuery(void* q);
        void issueTimestamp(void*& q);
        void collectFinished();
        void writeFrame(const FrameRecord& rec, uint64_t freq);
        void logSummary();

        Config m_config;
        bool m_initialized = false;
        bool m_frameActive = false;
        uint64_t m_frameIndex = 0;
        uint64_t m_frequency = 0;

        FrameRecord m_current;
        std::vector<size_t> m_openScopes;
        std::vector<FrameRecord> m_pending; // frames whose results the GPU has not finalized yet
        std::vector<void*> m_freeQueries;
        std::vector<void*> m_allQueries;

        std::ofstream m_csv;
        std::unordered_map<std::string, Accum> m_accum;
        std::vector<std::string> m_accumOrder;
        uint64_t m_framesInAccum = 0;
    };

    // RAII helper: times everything between construction and destruction.
    class ScopedGpuTimer
    {
    public:
        explicit ScopedGpuTimer(const char* name)
        {
            auto& p = GpuProfiler::instance();
            m_active = p.isEnabled();
            if (m_active)
            {
                p.beginScope(name);
            }
        }
        ~ScopedGpuTimer()
        {
            if (m_active)
            {
                GpuProfiler::instance().endScope();
            }
        }
        ScopedGpuTimer(const ScopedGpuTimer&) = delete;
        ScopedGpuTimer& operator=(const ScopedGpuTimer&) = delete;

    private:
        bool m_active = false;
    };
} // namespace nau::gpu_profiler

#define NAU_GPU_TIMER_CONCAT_(a, b) a##b
#define NAU_GPU_TIMER_CONCAT(a, b) NAU_GPU_TIMER_CONCAT_(a, b)
#define NAU_GPU_TIMER(name) ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_TIMER_CONCAT(nauGpuTimer_, __LINE__)(name)
