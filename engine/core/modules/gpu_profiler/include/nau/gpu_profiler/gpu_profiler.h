// gpu_profiler.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

// GPU timestamp profiler for comparing a scene rendered on one and on two GPUs.
//
// The public interface uses plain C++ types. The implementation (EASTL) is hidden.
//
// Timestamp sources. Every queue that executes measured work has its own source (ITimestampSource), registered with
// the GPU it runs on and a queue name: Nau's driver on GPU 0 (always source PrimarySource), and raw D3D12 queues of
// the AO producer. Timestamps of different sources are never subtracted from each other: they come from different
// clocks, possibly with different frequencies. Only the clock calibration to the CPU counter (QueryPerformanceCounter)
// relates them.
//
// Frames. beginFrame()/endFrame() are called on the render thread around the frame; beginFrame() opens the "Frame"
// scope on GPU 0. The results of a frame are read without waiting, once the GPU has written them: frames wait in a
// queue of pending frames per GPU (Config::maxPendingFrames). When that queue is full the frame is not measured on that
// GPU (counted as skipped); a frame whose results do not arrive in time (Config::timeoutFrames / timeoutMs) is dropped
// and logged, so a lost device never blocks the profiler.
//
// Transfers between GPUs. Each half is timed on the GPU and the queue that executes it. The halves of one transfer are
// matched by name and token (for example the number of the frame that started it), not by the current frame, because
// an asynchronous transfer may take several frames:
//
//     { NAU_GPU_TRANSFER_SRC("csm_atlas", 1, 0, token, source1); list1->CopyResource(shared1, atlas1); }
//     ... frames later, on the copy queue of GPU 0:
//     { NAU_GPU_TRANSFER_DST("csm_atlas", 1, 0, token, copySource0); list0->CopyResource(local0, shared0); }
//     ... and in the frame that uses the result:
//     GpuProfiler::instance().markTransferConsumed("csm_atlas", token);
//
// The CSV gets name/src, name/dst, name/sum (src + dst: pure GPU work) and name/span (from the start of src to the end
// of dst on the CPU clock, through the clock calibration of both queues: includes the waits for fences and queues;
// written only when both are calibrated). All rows of a transfer carry the frame that started it and its age: frames
// between that frame and the frame that used the result. The profiler adds no synchronization between GPUs; it only
// observes the application's transfers.
//
// Mode of a frame. setPlacement() records where each feature actually ran in the frame; "mode" is "2gpu" when at least
// one feature used a result of GPU 1, and "features" lists the placements ("csm=gpu1;dof=off"). Every row of a frame
// (GPU, CPU and transfers started in it) carries them.
//
// Run metadata. <csv name>.json is written next to the CSV at start and completed at shutdown: commit and local
// changes of the build, preset and configuration, adapters and driver versions, resolution (with changes), vsync,
// warmup, start time, duration, power source, frame counts per mode and features, counters.
//
// CPU time. endFrame() writes "cpu_frame": the time between the ends of two neighbouring frames (the present follows
// endFrame()). NAU_CPU_TIMER times work on the render thread. The 1 GPU / 2 GPU comparison is based on the CPU frame
// time; GPU times explain where the time goes.
//
// Configuration (environment variables, read by init()):
//   NAU_GPU_PROFILER=1                  enable (disabled by default; then every macro costs one bool check)
//   NAU_GPU_PROFILER_CSV=<path>         CSV output (default gpu_timings.csv in the working directory)
//   NAU_GPU_PROFILER_LOG_EVERY=<N>      averaged timings in the log every N frames (default 300, 0 = never)
//   NAU_GPU_PROFILER_WARMUP=<N>         the first N frames are not measured (default 120)
//   NAU_GPU_PROFILER_MAX_PENDING=<N>    frames waiting for the results of one GPU (default 8)
//   NAU_GPU_PROFILER_TIMEOUT_FRAMES=<N> a pending frame is dropped after N frames (default 64)
//   NAU_GPU_PROFILER_TIMEOUT_MS=<N>     ... or after N milliseconds (default 2000)
//
// All calls must come from one thread, the render thread.
#pragma once

#include <cstdint>

#if defined(_WIN32) && defined(NAU_GPU_PROFILER_SHARED)
    #if defined(NAU_GPU_PROFILER_BUILD)
        #define NAU_GPU_PROFILER_API __declspec(dllexport)
    #else
        #define NAU_GPU_PROFILER_API __declspec(dllimport)
    #endif
#else
    #define NAU_GPU_PROFILER_API
#endif

namespace nau::gpu_profiler
{
    using GpuIndex = uint32_t;
    inline constexpr GpuIndex PrimaryGpu = 0;
    inline constexpr GpuIndex SecondaryGpu = 1;
    inline constexpr GpuIndex MaxGpus = 2;

    // Registered timestamp source (one GPU queue).
    using SourceId = uint32_t;
    inline constexpr SourceId InvalidSource = ~0u;
    // Nau's driver on GPU 0, registered first by Graphics.
    inline constexpr SourceId PrimarySource = 0;
    // The source of the GPU that can record right now (see GpuProfiler::findSource).
    inline constexpr SourceId AutoSource = ~1u;

    // Token of a transfer whose halves run in the same frame: the current frame index.
    inline constexpr uint64_t FrameToken = ~0ull;

    enum class TransferHalf : uint8_t
    {
        Source,
        Destination
    };

    /** @brief Supplies timestamps from a single GPU command stream. */
    class ITimestampSource
    {
    public:
        virtual ~ITimestampSource() = default;

        /** @brief Returns timestamp ticks per second, or 0 if unavailable. */
        virtual uint64_t getFrequency() = 0;
        /** @brief Returns whether the source can record a timestamp now. */
        virtual bool canIssue()
        {
            return true;
        }
        /**
         * @brief Writes a timestamp into the command stream.
         * @param[in,out] query Query to reuse after readback, or nullptr to acquire a new query.
         */
        virtual void issue(void*& query) = 0;
        /**
         * @brief Polls a timestamp without waiting for the GPU.
         * @param query Previously issued query.
         * @param[out] value Timestamp ticks if the result is ready.
         * @return True if a result was read.
         */
        virtual bool tryGetValue(void* query, uint64_t& value) = 0;
        /**
         * @brief Releases a query from profiler use. The GPU may still write it.
         * @param query Query to release.
         */
        virtual void release(void* query) = 0;
        /**
         * @brief Samples the GPU and CPU counters at the same instant.
         * @param[out] gpuTicks GPU timestamp sample.
         * @param[out] cpuTicks CPU counter sample.
         * @param[out] cpuFrequency CPU ticks per second, or 0 for QueryPerformanceCounter.
         * @return True if calibration was available.
         */
        virtual bool getClockCalibration(uint64_t& gpuTicks, uint64_t& cpuTicks, uint64_t& cpuFrequency)
        {
            return false;
        }
    };

    // Where a feature ran in a frame ("features" column, e.g. "csm=gpu1;dof=gpu0").
    enum class Placement : uint8_t
    {
        Off,
        Gpu0,
        Gpu1
    };

    enum class LogLevel : uint8_t
    {
        Info,
        Warning,
        Error
    };

    using LogSink = void (*)(LogLevel level, const char* message);

    struct Config
    {
        bool enabled = false;
        char csvPath[260] = "gpu_timings.csv";
        uint32_t logEveryNFrames = 300;
        uint32_t warmupFrames = 120;
        uint32_t maxPendingFrames = 8;
        uint32_t timeoutFrames = 64;
        uint32_t timeoutMs = 2000;
    };

    struct Counters
    {
        uint64_t framesMeasured = 0;            // frames of one GPU whose results were written
        uint64_t framesSkipped = 0;             // the pending queue of a GPU was full
        uint64_t framesTimedOut = 0;            // results did not arrive in time
        uint64_t framesInFlightAtShutdown = 0;  // dropped at shutdown, not an error
        uint64_t scopesUnmeasured = 0;          // the source could not record, or the scope was not closed
        uint64_t recordsDropped = 0;            // CSV records lost
        uint64_t transfersComplete = 0;         // both halves measured
        uint64_t transfersIncomplete = 0;       // a half did not arrive in time and the bridge did not discard the transfer
        uint64_t transfersUnused = 0;           // the result was never used (discarded by the bridge or never consumed)
        uint64_t transfersInFlightAtShutdown = 0;
    };

    /** @brief Collects queue-local GPU timings and render-thread measurements without waiting for results. */
    class NAU_GPU_PROFILER_API GpuProfiler
    {
    public:
        static GpuProfiler& instance();

        /** @brief Initializes from the profiler environment variables if enabled. */
        void init();
        void init(const Config& config);
        /** @brief Collects available results and closes the run. Keep sources alive until this returns. */
        void shutdown();

        /**
         * @brief Sets where profiler messages are sent. May be called before init().
         * @param sink Log callback. The default sink writes to stderr.
         */
        void setLogSink(LogSink sink);

        bool isEnabled() const
        {
            return m_enabled;
        }
        const Config& config() const;
        const Counters& counters() const;

        /**
         * @brief Registers a queue timestamp source.
         * @param gpu GPU that owns the queue.
         * @param queueName Queue label written to the CSV.
         * @param source Borrowed source, which must outlive its registration.
         * @return Source ID, or InvalidSource when profiling or timestamps are unavailable.
         */
        SourceId registerSource(GpuIndex gpu, const char* queueName, ITimestampSource* source);
        /**
         * @brief Unregisters the source and drops its pending scopes.
         * @param source Registered source ID.
         */
        void unregisterSource(SourceId source);
        /**
         * @brief Finds a source that can record now, preferring an open native command list.
         * @param gpu GPU the source must belong to.
         * @return A source ID, or InvalidSource if none can record.
         */
        SourceId findSource(GpuIndex gpu) const;

        void beginFrame();
        void endFrame();
        uint64_t frameIndex() const;

        void beginScope(const char* name, SourceId source);
        void endScope(SourceId source);

        /**
         * @brief Begins timing one half of a cross-GPU transfer. Close it with endScope(source).
         * @param name Transfer name used to match the two halves.
         * @param srcGpu GPU producing the payload.
         * @param dstGpu GPU receiving the payload.
         * @param half Which half this command stream executes.
         * @param token Transfer identifier shared by both halves. FrameToken uses the current frame.
         * @param source Timestamp source for the queue executing this half.
         * @param bytes Payload size in bytes, or 0 if unknown.
         */
        void beginTransfer(const char* name, GpuIndex srcGpu, GpuIndex dstGpu, TransferHalf half, uint64_t token, SourceId source, uint64_t bytes = 0);
        /**
         * @brief Marks the transfer result as used in the current frame, which determines its age.
         * @param name Recorded transfer name.
         * @param token Recorded transfer identifier. FrameToken uses the current frame.
         */
        void markTransferConsumed(const char* name, uint64_t token = FrameToken);
        /**
         * @brief Records a discarded result as unused without waiting for a missing half.
         * @param name Recorded transfer name.
         * @param token Recorded transfer identifier.
         */
        void markTransferDiscarded(const char* name, uint64_t token);

        /**
         * @brief Begins measuring render-thread time with QueryPerformanceCounter.
         * @param name Scope name written to the CSV.
         */
        void beginCpuScope(const char* name);
        void endCpuScope();

        /**
         * @brief Records the actual placement of a feature for the current frame.
         * beginFrame() clears the feature list. A frame uses the 2gpu label when a feature used GPU1.
         * @param feature Name written to the features column.
         * @param placement Execution location observed for this frame.
         */
        void setPlacement(const char* feature, Placement placement);

        // Run metadata, written next to the CSV (<csv name>.json): adapter of a GPU, any key/value of the run
        // (vsync, driver...), the render resolution (every change is kept with its frame).
        void setGpuName(GpuIndex gpu, const char* name);
        void setRunInfo(const char* key, const char* value);
        void setResolution(uint32_t width, uint32_t height);

        struct Impl;

    private:
        GpuProfiler() = default;
        ~GpuProfiler();

        bool m_enabled = false;
        Impl* m_impl = nullptr;
    };

    class ScopedGpuTimer
    {
    public:
        ScopedGpuTimer(const char* name, const SourceId source)
        {
            GpuProfiler& profiler = GpuProfiler::instance();
            if (profiler.isEnabled())
            {
                m_source = source;
                profiler.beginScope(name, m_source);
            }
        }

        // The int tag selects the GPU form (NAU_GPU_TIMER_ON).
        ScopedGpuTimer(const char* name, const GpuIndex gpu, int)
        {
            GpuProfiler& profiler = GpuProfiler::instance();
            if (profiler.isEnabled())
            {
                m_source = profiler.findSource(gpu);
                profiler.beginScope(name, m_source);
            }
        }

        ~ScopedGpuTimer()
        {
            if (m_source != InvalidSource)
            {
                GpuProfiler::instance().endScope(m_source);
            }
        }

        ScopedGpuTimer(const ScopedGpuTimer&) = delete;
        ScopedGpuTimer& operator=(const ScopedGpuTimer&) = delete;

    private:
        SourceId m_source = InvalidSource;
    };

    // Arguments of NAU_GPU_TRANSFER_SRC/DST; token and source are optional.
    struct TransferArgs
    {
        const char* name = nullptr;
        GpuIndex srcGpu = PrimaryGpu;
        GpuIndex dstGpu = SecondaryGpu;
        uint64_t token = FrameToken;
        SourceId source = AutoSource;
        uint64_t bytes = 0;  // payload size, 0: unknown
    };

    class ScopedGpuTransfer
    {
    public:
        ScopedGpuTransfer(const TransferHalf half, const TransferArgs& args)
        {
            GpuProfiler& profiler = GpuProfiler::instance();
            if (profiler.isEnabled())
            {
                m_source = args.source != AutoSource
                               ? args.source
                               : profiler.findSource(half == TransferHalf::Source ? args.srcGpu : args.dstGpu);
                profiler.beginTransfer(args.name, args.srcGpu, args.dstGpu, half, args.token, m_source, args.bytes);
            }
        }

        ~ScopedGpuTransfer()
        {
            if (m_source != InvalidSource)
            {
                GpuProfiler::instance().endScope(m_source);
            }
        }

        ScopedGpuTransfer(const ScopedGpuTransfer&) = delete;
        ScopedGpuTransfer& operator=(const ScopedGpuTransfer&) = delete;

    private:
        SourceId m_source = InvalidSource;
    };

    class ScopedCpuTimer
    {
    public:
        explicit ScopedCpuTimer(const char* name)
        {
            GpuProfiler& profiler = GpuProfiler::instance();
            m_active = profiler.isEnabled();
            if (m_active)
            {
                profiler.beginCpuScope(name);
            }
        }

        ~ScopedCpuTimer()
        {
            if (m_active)
            {
                GpuProfiler::instance().endCpuScope();
            }
        }

        ScopedCpuTimer(const ScopedCpuTimer&) = delete;
        ScopedCpuTimer& operator=(const ScopedCpuTimer&) = delete;

    private:
        bool m_active = false;
    };
}  // namespace nau::gpu_profiler

#define NAU_GPU_PROFILER_CONCAT_(a, b) a##b
#define NAU_GPU_PROFILER_CONCAT(a, b) NAU_GPU_PROFILER_CONCAT_(a, b)

// Times the enclosing scope on GPU 0, through Nau's driver.
#define NAU_GPU_TIMER(name) \
    ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_PROFILER_CONCAT(nauGpuTimer_, __LINE__)(name, ::nau::gpu_profiler::PrimarySource)
// Times the enclosing scope on the source of `gpu` that records right now (a raw D3D12 source with an open command
// list, otherwise Nau's driver on GPU 0).
#define NAU_GPU_TIMER_ON(gpu, name) \
    ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_PROFILER_CONCAT(nauGpuTimer_, __LINE__)(name, gpu, 0)
// Times the enclosing scope on the given registered source.
#define NAU_GPU_TIMER_SRC(source, name) \
    ::nau::gpu_profiler::ScopedGpuTimer NAU_GPU_PROFILER_CONCAT(nauGpuTimer_, __LINE__)(name, source)
// Halves of a transfer from srcGpu to dstGpu, each timed by the GPU and queue doing it. Optional arguments: token
// (default: the current frame), source (default: the source of that GPU that records right now), bytes (payload).
#define NAU_GPU_TRANSFER_SRC(name, srcGpu, dstGpu, ...)                                        \
    ::nau::gpu_profiler::ScopedGpuTransfer NAU_GPU_PROFILER_CONCAT(nauGpuTransfer_, __LINE__)( \
        ::nau::gpu_profiler::TransferHalf::Source, ::nau::gpu_profiler::TransferArgs{name, srcGpu, dstGpu, __VA_ARGS__})
#define NAU_GPU_TRANSFER_DST(name, srcGpu, dstGpu, ...)                                        \
    ::nau::gpu_profiler::ScopedGpuTransfer NAU_GPU_PROFILER_CONCAT(nauGpuTransfer_, __LINE__)( \
        ::nau::gpu_profiler::TransferHalf::Destination, ::nau::gpu_profiler::TransferArgs{name, srcGpu, dstGpu, __VA_ARGS__})
// CPU time of the enclosing scope on the render thread.
#define NAU_CPU_TIMER(name) ::nau::gpu_profiler::ScopedCpuTimer NAU_GPU_PROFILER_CONCAT(nauCpuTimer_, __LINE__)(name)
