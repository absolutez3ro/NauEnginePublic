// Debug mode of the GPU profiler that checks timestamps of a second GPU without any multi-GPU code of the engine.
//
//   NAU_GPU_PROFILER_SELFTEST=1   copy on the second GPU (requires NAU_GPU_PROFILER=1)
//   NAU_GPU_PROFILER_SELFTEST=2   also a copy from the second GPU to the engine's GPU
//
// 1: creates its own D3D12 device on a GPU other than the engine's one (a hardware adapter with another LUID, WARP if
//    there is none), connects it to the profiler as GPU 1 and every frame copies a large buffer on it under
//    NAU_GPU_TIMER_ON(1, "copy_test"). The rows appear in the profiler CSV with gpu=1 next to the engine passes of GPU 0.
//
// 2: in addition every frame copies a buffer from GPU 1 to GPU 0 through a cross-adapter heap, synchronized with a
//    cross-adapter fence, timed with NAU_GPU_TRANSFER_SRC/DST("transfer_test", 1, 0). The GPU 0 half runs on a queue of
//    the self-test on the engine's adapter. One queue per GPU is measured, so in this mode GPU 0 is measured on that
//    queue and the engine passes are not measured.
#pragma once

#include <EASTL/array.h>
#include <d3d12.h>
#include <dxgi1_6.h>
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

        enum class Mode
        {
            Copy = 1,
            Transfer = 2
        };

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
            // False for a device that is shared with the engine: its debug messages belong to the engine.
            bool ownsDebugMessages = true;

            bool init(IUnknown* adapter, const char* name);
            void shutdown();
            ID3D12GraphicsCommandList* begin(uint32_t frame);
            void submit(uint32_t frame);
            void waitIdle();
            void logDebugMessages();
        };

        bool initTransfer(IDXGIFactory4* factory, const LUID& engineLuid);
        void release();

        bool m_initialized = false;
        uint32_t m_framesBeforeInit = 0;
        bool m_active = false;
        uint32_t m_frame = 0;

        Mode m_mode = Mode::Copy;

        GpuContext m_secondary;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_copySrc;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_copyDst;

        // Mode::Transfer: the buffer in the cross-adapter heap as seen by each device, and the shared fence.
        GpuContext m_primary;
        Microsoft::WRL::ComPtr<ID3D12Heap> m_sharedHeapPrimary;
        Microsoft::WRL::ComPtr<ID3D12Heap> m_sharedHeapSecondary;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedPrimary;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedSecondary;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_transferDst;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFencePrimary;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFenceSecondary;
        uint64_t m_sharedFenceValue = 0;
    };
}  // namespace nau::gpu_profiler
