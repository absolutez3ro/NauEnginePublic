// Debug mode of the GPU profiler that checks timestamps of a second GPU without any multi-GPU code of the engine.
//
//   NAU_GPU_PROFILER_SELFTEST=1   requires NAU_GPU_PROFILER=1
//
// Creates its own D3D12 device on a GPU other than the engine's one (a hardware adapter with another LUID, WARP if
// there is none), connects it to the profiler as GPU 1 and every frame copies a large buffer on it under
// NAU_GPU_TIMER_ON(1, "copy_test"). The rows appear in the profiler CSV with gpu=1 next to the engine passes of GPU 0.
#pragma once

#include <EASTL/array.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "d3d12_timestamp_source.h"

namespace nau::gpu_profiler
{
    class SelfTest
    {
    public:
        static SelfTest& instance();

        // Call on the render thread every frame after GpuProfiler::init(); starts the self-test a few frames later
        // if it is requested.
        void init();
        // Call between GpuProfiler::beginFrame() and GpuProfiler::endFrame().
        void tick();
        // Call before GpuProfiler::shutdown().
        void shutdown();

    private:
        static constexpr uint32_t FramesInFlight = 3;
        static constexpr uint32_t InitAfterFrames = 10;

        // A device with one direct queue; everything submitted to the queue is timed by `timestamps`.
        struct GpuContext
        {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            // A plain array: eastl::array takes element addresses with operator&, which ComPtr overloads to release.
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators[FramesInFlight];
            eastl::array<uint64_t, FramesInFlight> allocatorFence = {};
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            uint64_t fenceValue = 0;
            D3D12TimestampSource timestamps;

            bool init(IUnknown* adapter, const char* name);
            void shutdown();
            ID3D12GraphicsCommandList* begin(uint32_t frame);
            void submit(uint32_t frame);
            void waitIdle();
            void logDebugMessages();
        };

        void release();

        bool m_initialized = false;
        uint32_t m_framesBeforeInit = 0;
        bool m_active = false;
        uint32_t m_frame = 0;

        GpuContext m_secondary;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_copySrc;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_copyDst;
    };
}  // namespace nau::gpu_profiler
