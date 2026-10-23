// gpu_profiler.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
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
//
// Copy between GPUs. Each half of a transfer is timed on its own GPU, timestamps of different GPUs are never
// subtracted from each other:
//
//     // GPU 1, source: copy from local memory into the cross-adapter resource
//     {
//         NAU_GPU_TRANSFER_SRC("shadow_copy", 1, 0);
//         list1->CopyBufferRegion(sharedOn1, 0, local1, 0, size);
//     }
//     // signal the shared fence on GPU 1, wait for it on GPU 0, then on GPU 0, destination:
//     {
//         NAU_GPU_TRANSFER_DST("shadow_copy", 1, 0);
//         list0->CopyBufferRegion(local0, 0, sharedOn0, 0, size);
//     }
//
// Both halves must be recorded in the same profiler frame (between beginFrame() and endFrame()). The CSV gets:
//   shadow_copy/src   gpu=1     time of the source half on GPU 1
//   shadow_copy/dst   gpu=0     time of the destination half on GPU 0
//   shadow_copy/sum   gpu=1>0   src + dst: pure GPU work of the transfer
//   shadow_copy/span  gpu=1>0   from the start of src to the end of dst, both moved to the CPU clock with
//                               ID3D12CommandQueue::GetClockCalibration; includes the wait for the shared fence.
//                               Written only when both GPUs provide clock calibration.
#pragma once

#include <EASTL/array.h>
#include <EASTL/optional.h>
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
        // False while the GPU has no command stream to write timestamps into; scopes opened then are not measured.
        virtual bool canIssue()
        {
            return true;
        }
        // Writes a timestamp into the GPU command stream. A null query is created by the source on first use.
        virtual void issue(void*& query) = 0;
        // Returns true and the timestamp once the GPU has written it.
        virtual bool tryGetValue(void* query, uint64_t& value) = 0;
        virtual void release(void* query) = 0;
        // A GPU timestamp and the CPU counter (QueryPerformanceCounter ticks, cpuFrequency per second) sampled at the
        // same moment. Lets timestamps of different GPUs be moved to one CPU time scale. False if not supported.
        virtual bool getClockCalibration(uint64_t& gpuTicks, uint64_t& cpuTicks, uint64_t& cpuFrequency)
        {
            return false;
        }
    };

    enum class TransferHalf
    {
        Source,
        Destination
    };

    enum class GpuMode
    {
        Single,
        Multi
    };

    const char* toString(GpuMode mode);

    struct TimestampCalibration
    {
        bool valid = false;
        uint64_t gpuTicks = 0;
        uint64_t cpuTicks = 0;
        uint64_t cpuFrequency = 0;
    };

    /** @brief Resolved timestamps from one queue, read only after that queue's fence completed. */
    struct TransferTiming
    {
        uint64_t frame = 0;
        uint64_t begin = 0;
        uint64_t end = 0;
        uint64_t frequency = 0;
        TimestampCalibration calibration;
    };

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
        // Opens the scope of one half of a copy from srcGpu to dstGpu on the GPU doing that half; close it with
        // endScope() on the same GPU.
        void beginTransfer(const char* name, GpuIndex srcGpu, GpuIndex dstGpu, TransferHalf half);

        uint64_t frameIndex() const
        {
            return m_frameIndex;
        }
        // Render-thread API for independently driven COPY queues. Uses the supplied queue frequency/calibration,
        // leaving the registered DIRECT sources unchanged. Both halves must carry the sending frame's index.
        void recordTransferHalf(const char* name, GpuIndex srcGpu, GpuIndex dstGpu, TransferHalf half, const TransferTiming& timing);

    private:
        struct TransferInfo
        {
            eastl::string name;
            GpuIndex src = 0;
            GpuIndex dst = 0;
            TransferHalf half = TransferHalf::Source;
        };

        struct Scope
        {
            eastl::string name;
            void* begin = nullptr;
            void* end = nullptr;
            eastl::optional<TransferInfo> transfer;
        };

        using Calibration = TimestampCalibration;

        struct FrameRecord
        {
            uint64_t frame = 0;
            Calibration calibration;
            eastl::vector<Scope> scopes;
        };

        // Halves of one transfer in one frame; they come from different GPUs at different times.
        struct TransferHalfResult
        {
            bool ready = false;
            uint64_t begin = 0;
            uint64_t end = 0;
            double ms = 0.0;
            uint64_t frequency = 0;
            Calibration calibration;
        };

        struct TransferRecord
        {
            uint64_t frame = 0;
            TransferInfo info;
            eastl::array<TransferHalfResult, 2> halves;  // indexed by TransferHalf
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
        void writeRow(uint64_t frame, const eastl::string& pass, const eastl::string& gpu, double ms);
        void accumulate(GpuState& state, const eastl::string& name, double ms);
        void addTransferHalf(uint64_t frequency, const FrameRecord& rec, const Scope& scope, uint64_t begin, uint64_t end, double ms);
        void writeTransfer(const TransferRecord& rec);
        void dropStaleTransfers();
        void logSummary(GpuIndex gpu);

        Config m_config;
        bool m_initialized = false;
        uint64_t m_frameIndex = 0;
        eastl::array<GpuState, MaxGpus> m_gpus;
        eastl::vector<TransferRecord> m_transfers;  // waiting for the other half
        std::ofstream m_csv;
    };

    // RAII helper: times one half of a copy between GPUs on the GPU doing that half (see the top of this file).
    class ScopedGpuTransfer
    {
    public:
        ScopedGpuTransfer(const char* name, GpuIndex srcGpu, GpuIndex dstGpu, TransferHalf half) :
            m_gpu(half == TransferHalf::Source ? srcGpu : dstGpu)
        {
            auto& p = GpuProfiler::instance();
            m_active = p.isEnabled();
            if (m_active)
            {
                p.beginTransfer(name, srcGpu, dstGpu, half);
            }
        }
        ~ScopedGpuTransfer()
        {
            if (m_active)
            {
                GpuProfiler::instance().endScope(m_gpu);
            }
        }
        ScopedGpuTransfer(const ScopedGpuTransfer&) = delete;
        ScopedGpuTransfer& operator=(const ScopedGpuTransfer&) = delete;

    private:
        GpuIndex m_gpu = PrimaryGpu;
        bool m_active = false;
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
// Times the source half of a copy from srcGpu to dstGpu; the work must be recorded through srcGpu's timestamp source.
#define NAU_GPU_TRANSFER_SRC(name, srcGpu, dstGpu) ::nau::gpu_profiler::ScopedGpuTransfer NAU_GPU_TIMER_CONCAT(nauGpuTransfer_, __LINE__)(name, srcGpu, dstGpu, ::nau::gpu_profiler::TransferHalf::Source)
// Times the destination half of a copy from srcGpu to dstGpu; the work must be recorded through dstGpu's source.
#define NAU_GPU_TRANSFER_DST(name, srcGpu, dstGpu) ::nau::gpu_profiler::ScopedGpuTransfer NAU_GPU_TIMER_CONCAT(nauGpuTransfer_, __LINE__)(name, srcGpu, dstGpu, ::nau::gpu_profiler::TransferHalf::Destination)
