// GPU timestamp profiler: measures GPU time of render passes (framegraph nodes) and of the whole frame.
// Every GPU is measured separately with its own timestamp source and its own timestamp frequency.
// Results are written to a CSV file (frame,pass,gpu,ms,mode) and periodically summarized into the log.
//
// Configuration (environment variables):
//   NAU_GPU_PROFILER=1              enable the profiler (disabled by default)
//   NAU_GPU_PROFILER_CSV=<path>     CSV output path (default: gpu_timings.csv in the working directory)
//   NAU_GPU_PROFILER_LOG_EVERY=<N>  print averaged timings to the log every N frames (default: 120, 0 = never)
//
// The "1 GPU / 2 GPU" mode is read from the application config, key dx12/multiGpu (the same switch that enables
// the secondary GPU device). It goes to the "mode" column, so runs in both modes can be compared on one scene.
#pragma once

#include <EASTL/array.h>
#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>

#include <cstdint>
#include <fstream>

namespace nau::gpu_profiler
{
    using GpuIndex = uint32_t;
    inline constexpr GpuIndex PrimaryGpu = 0;
    inline constexpr GpuIndex SecondaryGpu = 1;
    inline constexpr GpuIndex MaxGpus = 2;

    // Source of timestamps for one GPU. Timestamps of different GPUs tick at different frequencies and come from
    // unrelated clocks, so the profiler never subtracts a timestamp of one GPU from a timestamp of another.
    class ITimestampSource
    {
    public:
        virtual ~ITimestampSource() = default;

        // Timestamp ticks per second, 0 if the GPU cannot provide timestamps.
        virtual uint64_t getFrequency() = 0;
        // Writes a timestamp into the GPU command stream. A null query is created by the source on first use.
        virtual void issue(void*& query) = 0;
        // Returns true and the timestamp once the GPU has written it.
        virtual bool tryGetValue(void* query, uint64_t& value) = 0;
        virtual void release(void* query) = 0;
    };

    enum class GpuMode
    {
        Single,
        Multi
    };

    const char* toString(GpuMode mode);

    struct Config
    {
        bool enabled = false;
        eastl::string csvPath = "gpu_timings.csv";
        uint32_t logEveryNFrames = 120;
        GpuMode mode = GpuMode::Single;
    };

    class GpuProfiler
    {
    public:
        static GpuProfiler& instance();

        // Reads the configuration on first use and connects the engine driver as the timestamp source of GPU 0.
        void init();
        // Starts with an explicit configuration and no GPUs connected; timestamp sources are added with registerGpu().
        void init(const Config& config);
        void shutdown();

        // Connects the timestamp source of a GPU. The source must stay alive until unregisterGpu() or shutdown().
        bool registerGpu(GpuIndex gpu, ITimestampSource* source);
        void unregisterGpu(GpuIndex gpu);
        bool hasGpu(GpuIndex gpu) const;

        bool isEnabled() const
        {
            return m_config.enabled;
        }
        const Config& config() const
        {
            return m_config;
        }

        // Must be called on the render thread while the GPU is owned (between ACQUIRE/RELEASE_OWNERSHIP).
        // beginFrame() also opens the "Frame" scope on GPU 0.
        void beginFrame();
        void endFrame();

        void beginScope(const char* name, GpuIndex gpu = PrimaryGpu);
        void endScope(GpuIndex gpu = PrimaryGpu);

    private:
        struct Scope
        {
            eastl::string name;
            void* begin = nullptr;
            void* end = nullptr;
        };

        struct FrameRecord
        {
            uint64_t frame = 0;
            eastl::vector<Scope> scopes;
        };

        struct Accum
        {
            double sumMs = 0.0;
            double maxMs = 0.0;
            uint32_t count = 0;
        };

        struct GpuState
        {
            ITimestampSource* source = nullptr;
            uint64_t frequency = 0;

            bool frameActive = false;
            FrameRecord current;
            eastl::vector<size_t> openScopes;
            eastl::vector<FrameRecord> pending;  // frames whose results the GPU has not finalized yet
            eastl::vector<void*> freeQueries;
            eastl::vector<void*> allQueries;

            eastl::unordered_map<eastl::string, Accum> accum;
            eastl::vector<eastl::string> accumOrder;
            uint64_t framesInAccum = 0;
        };

        void* acquireQuery(GpuState& state);
        void issueTimestamp(GpuState& state, void*& query);
        void collectFinished(GpuIndex gpu);
        void writeFrame(GpuIndex gpu, const FrameRecord& rec);
        void logSummary(GpuIndex gpu);

        Config m_config;
        bool m_initialized = false;
        uint64_t m_frameIndex = 0;
        eastl::array<GpuState, MaxGpus> m_gpus;
        std::ofstream m_csv;
    };

    // RAII helper: times everything between construction and destruction on the given GPU.
    class ScopedGpuTimer
    {
    public:
        explicit ScopedGpuTimer(const char* name, GpuIndex gpu = PrimaryGpu) :
            m_gpu(gpu)
        {
            auto& p = GpuProfiler::instance();
            m_active = p.isEnabled();
            if (m_active)
            {
                p.beginScope(name, m_gpu);
            }
        }
        ~ScopedGpuTimer()
        {
            if (m_active)
            {
                GpuProfiler::instance().endScope(m_gpu);
            }
        }
        ScopedGpuTimer(const ScopedGpuTimer&) = delete;
        ScopedGpuTimer& operator=(const ScopedGpuTimer&) = delete;

    private:
        GpuIndex m_gpu = PrimaryGpu;
        bool m_active = false;
    };
}  // namespace nau::gpu_profiler

#define NAU_GPU_TIMER_CONCAT_(a, b) a##b
#define NAU_GPU_TIMER_CONCAT(a, b) NAU_GPU_TIMER_CONCAT_(a, b)
// Times the enclosing scope on GPU 0.
#define NAU_GPU_TIMER(name) ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_TIMER_CONCAT(nauGpuTimer_, __LINE__)(name)
// Times the enclosing scope on the given GPU; its work must be recorded through that GPU's timestamp source.
#define NAU_GPU_TIMER_ON(gpu, name) \
    ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_TIMER_CONCAT(nauGpuTimer_, __LINE__)(name, gpu)
