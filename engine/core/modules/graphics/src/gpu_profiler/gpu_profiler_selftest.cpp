#include "gpu_profiler_selftest.h"

#include <EASTL/vector.h>
#include <dxgi1_6.h>

#include <cstdlib>

#include "nau/3d/dag_drv3d.h"
#include "nau/diag/logging.h"

namespace nau::gpu_profiler
{
    namespace
    {
        // Large enough for the copy to take a measurable time (tens of MB).
        constexpr uint64_t CopyBufferSize = 64ull * 1024 * 1024;
        // The cross-adapter heap lives in system memory, the copy goes over the bus.
        constexpr uint64_t TransferBufferSize = 32ull * 1024 * 1024;

        eastl::string toUtf8(const wchar_t* s)
        {
            eastl::string r;
            for (; *s; ++s)
            {
                r += (*s < 128) ? char(*s) : '?';
            }
            return r;
        }

        bool getEngineAdapterLuid(LUID& luid)
        {
            auto* unknown = static_cast<IUnknown*>(d3d::get_device(d3d::PRIMARY_GPU));
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            if (!unknown || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device))))
            {
                return false;
            }
            luid = device->GetAdapterLuid();
            return true;
        }

        // A hardware adapter that is not the engine's one, or WARP when there is no such adapter.
        Microsoft::WRL::ComPtr<IDXGIAdapter1> findSecondaryAdapter(IDXGIFactory4* factory, const LUID& engineLuid)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc = {};
                adapter->GetDesc1(&desc);
                const bool sameAsEngine = desc.AdapterLuid.LowPart == engineLuid.LowPart && desc.AdapterLuid.HighPart == engineLuid.HighPart;
                if (!sameAsEngine && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                {
                    return adapter;
                }
                adapter.Reset();
            }

            NAU_LOG_WARNING("GPU profiler self-test: no second hardware GPU, using WARP");
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
            {
                return nullptr;
            }
            return adapter;
        }

        D3D12_RESOURCE_DESC bufferDesc(uint64_t size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;
            return desc;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            const D3D12_RESOURCE_DESC desc = bufferDesc(size);

            // Buffers live in the COMMON state and are promoted to COPY_SOURCE / COPY_DEST by the copy itself.
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&buffer))))
            {
                return nullptr;
            }
            return buffer;
        }
    }  // namespace

    bool SelfTest::GpuContext::init(IUnknown* adapter, const char* name)
    {
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create D3D12 device on {}", name);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create command queue on {}", name);
            return false;
        }
        queue->SetName(L"GPU profiler self-test");

        for (auto& allocator : allocators)
        {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
            {
                NAU_LOG_WARNING("GPU profiler self-test: cannot create command allocator on {}", name);
                return false;
            }
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create command list on {}", name);
            return false;
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create fence on {}", name);
            return false;
        }

        return timestamps.init(device.Get(), queue.Get());
    }

    void SelfTest::GpuContext::shutdown()
    {
        if (queue && fence)
        {
            waitIdle();
        }
        timestamps.shutdown();
        list.Reset();
        for (auto& allocator : allocators)
        {
            allocator.Reset();
        }
        allocatorFence = {};
        fence.Reset();
        fenceValue = 0;
        queue.Reset();
        device.Reset();
    }

    ID3D12GraphicsCommandList* SelfTest::GpuContext::begin(uint32_t frame)
    {
        const uint32_t i = frame % FramesInFlight;
        if (fence->GetCompletedValue() < allocatorFence[i])
        {
            fence->SetEventOnCompletion(allocatorFence[i], nullptr);  // blocks until the GPU is done with the allocator
        }
        allocators[i]->Reset();
        list->Reset(allocators[i].Get(), nullptr);
        timestamps.setCommandList(list.Get());
        return list.Get();
    }

    void SelfTest::GpuContext::submit(uint32_t frame)
    {
        timestamps.resolve(list.Get());
        timestamps.setCommandList(nullptr);
        list->Close();

        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        timestamps.submitted();

        queue->Signal(fence.Get(), ++fenceValue);
        allocatorFence[frame % FramesInFlight] = fenceValue;

        if (ownsDebugMessages)
        {
            logDebugMessages();
        }
    }

    void SelfTest::GpuContext::logDebugMessages()
    {
        // Only present when the engine has enabled the D3D12 debug layer; it applies to every device of the process.
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device.As(&infoQueue)))
        {
            return;
        }

        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            infoQueue->GetMessage(i, nullptr, &size);
            eastl::vector<char> buffer(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                NAU_LOG_WARNING("GPU profiler self-test: D3D12 debug layer [#{}]: {}", int(message->ID), message->pDescription);
            }
        }
        infoQueue->ClearStoredMessages();
    }

    void SelfTest::GpuContext::waitIdle()
    {
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, nullptr);
    }

    SelfTest& SelfTest::instance()
    {
        static SelfTest selfTest;
        return selfTest;
    }

    void SelfTest::init()
    {
        if (m_initialized)
        {
            return;
        }
        // Creating a device takes a while. On the first frames the render thread declares the render graph, which
        // touches ImGui while the game thread may render it, so the self-test keeps away from the first frames.
        if (++m_framesBeforeInit < InitAfterFrames)
        {
            return;
        }
        m_initialized = true;

        const char* env = std::getenv("NAU_GPU_PROFILER_SELFTEST");
        const int mode = env ? std::atoi(env) : 0;
        if (mode == 0 || !GpuProfiler::instance().isEnabled())
        {
            return;
        }
        m_mode = mode == int(Mode::Transfer) ? Mode::Transfer : Mode::Copy;

        LUID engineLuid = {};
        if (!getEngineAdapterLuid(engineLuid))
        {
            NAU_LOG_WARNING("GPU profiler self-test: the engine D3D12 device is not available");
            return;
        }

        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create DXGI factory");
            return;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter = findSecondaryAdapter(factory.Get(), engineLuid);
        if (!adapter)
        {
            NAU_LOG_WARNING("GPU profiler self-test: no adapter for GPU {}", SecondaryGpu);
            return;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const eastl::string name = toUtf8(desc.Description);

        if (!m_secondary.init(adapter.Get(), name.c_str()))
        {
            release();
            return;
        }
        m_copySrc = createBuffer(m_secondary.device.Get(), CopyBufferSize);
        m_copyDst = createBuffer(m_secondary.device.Get(), CopyBufferSize);
        if (!m_copySrc || !m_copyDst)
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create copy buffers on {}", name.c_str());
            release();
            return;
        }
        if (!GpuProfiler::instance().registerGpu(SecondaryGpu, &m_secondary.timestamps))
        {
            release();
            return;
        }

        m_active = true;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        const bool debugLayer = SUCCEEDED(m_secondary.device.As(&infoQueue));
        NAU_LOG_INFO("GPU profiler self-test: GPU {} is '{}', copy_test copies {} MB every frame, D3D12 debug layer {}", SecondaryGpu,
                     name.c_str(), CopyBufferSize / (1024 * 1024), debugLayer ? "on" : "off");

        if (m_mode == Mode::Transfer && !initTransfer(factory.Get(), engineLuid))
        {
            NAU_LOG_WARNING("GPU profiler self-test: copy between GPUs is not available, only copy_test runs");
            m_mode = Mode::Copy;
        }
    }

    bool SelfTest::initTransfer(IDXGIFactory4* factory, const LUID& engineLuid)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByLuid(engineLuid, IID_PPV_ARGS(&adapter))))
        {
            return false;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        const eastl::string name = toUtf8(desc.Description);

        // D3D12 returns the engine's own device for its adapter; the self-test only adds a queue to it.
        m_primary.ownsDebugMessages = false;
        if (!m_primary.init(adapter.Get(), name.c_str()))
        {
            return false;
        }

        // The heap is created on the destination GPU and opened on the source GPU; buffers are placed at offset 0.
        const D3D12_RESOURCE_DESC sharedDesc = bufferDesc(TransferBufferSize, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        const D3D12_RESOURCE_ALLOCATION_INFO allocation = m_primary.device->GetResourceAllocationInfo(0, 1, &sharedDesc);

        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = allocation.SizeInBytes;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = allocation.Alignment;
        heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        if (const HRESULT hr = m_primary.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_sharedHeapPrimary)); FAILED(hr))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create cross-adapter heap on {}, hr {:#x}", name.c_str(), uint32_t(hr));
            return false;
        }

        HANDLE handle = nullptr;
        if (FAILED(m_primary.device->CreateSharedHandle(m_sharedHeapPrimary.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot share cross-adapter heap");
            return false;
        }
        const HRESULT openHeap = m_secondary.device->OpenSharedHandle(handle, IID_PPV_ARGS(&m_sharedHeapSecondary));
        CloseHandle(handle);
        if (FAILED(openHeap))
        {
            NAU_LOG_WARNING("GPU profiler self-test: GPU {} cannot open the cross-adapter heap", SecondaryGpu);
            return false;
        }

        if (FAILED(m_primary.device->CreatePlacedResource(m_sharedHeapPrimary.Get(), 0, &sharedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&m_sharedPrimary))) ||
            FAILED(m_secondary.device->CreatePlacedResource(m_sharedHeapSecondary.Get(), 0, &sharedDesc, D3D12_RESOURCE_STATE_COMMON,
                                                            nullptr, IID_PPV_ARGS(&m_sharedSecondary))))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot place buffers in the cross-adapter heap");
            return false;
        }

        // One fence for both directions: GPU 1 signals odd values (source written), GPU 0 even ones (source read).
        if (FAILED(m_secondary.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                                   IID_PPV_ARGS(&m_sharedFenceSecondary))) ||
            FAILED(m_secondary.device->CreateSharedHandle(m_sharedFenceSecondary.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create cross-adapter fence");
            return false;
        }
        const HRESULT openFence = m_primary.device->OpenSharedHandle(handle, IID_PPV_ARGS(&m_sharedFencePrimary));
        CloseHandle(handle);
        if (FAILED(openFence))
        {
            NAU_LOG_WARNING("GPU profiler self-test: GPU {} cannot open the cross-adapter fence", PrimaryGpu);
            return false;
        }

        m_transferDst = createBuffer(m_primary.device.Get(), TransferBufferSize);
        if (!m_transferDst)
        {
            NAU_LOG_WARNING("GPU profiler self-test: cannot create transfer buffer on {}", name.c_str());
            return false;
        }

        // One queue per GPU is measured: GPU 0 switches from the engine queue to the self-test queue.
        GpuProfiler& profiler = GpuProfiler::instance();
        profiler.unregisterGpu(PrimaryGpu);
        if (!profiler.registerGpu(PrimaryGpu, &m_primary.timestamps))
        {
            return false;
        }

        NAU_LOG_INFO(
            "GPU profiler self-test: transfer_test copies {} MB from GPU {} to GPU {} ('{}') every frame; GPU {} is measured "
            "on the self-test queue, engine passes are not measured",
            TransferBufferSize / (1024 * 1024), SecondaryGpu, PrimaryGpu, name.c_str(), PrimaryGpu);
        return true;
    }

    void SelfTest::tick()
    {
        if (!m_active)
        {
            return;
        }

        const bool transfer = m_mode == Mode::Transfer;
        if (transfer && m_sharedFenceValue > 0)
        {
            // GPU 1 overwrites the cross-adapter buffer only after GPU 0 has read the previous frame's data.
            m_secondary.queue->Wait(m_sharedFenceSecondary.Get(), m_sharedFenceValue);
        }

        ID3D12GraphicsCommandList* list = m_secondary.begin(m_frame);
        {
            NAU_GPU_TIMER_ON(SecondaryGpu, "copy_test");
            list->CopyBufferRegion(m_copyDst.Get(), 0, m_copySrc.Get(), 0, CopyBufferSize);
        }
        if (transfer)
        {
            // Source half: GPU 1 writes into the cross-adapter buffer.
            NAU_GPU_TRANSFER_SRC("transfer_test", SecondaryGpu, PrimaryGpu);
            list->CopyBufferRegion(m_sharedSecondary.Get(), 0, m_copySrc.Get(), 0, TransferBufferSize);
        }
        m_secondary.submit(m_frame);

        if (transfer)
        {
            m_secondary.queue->Signal(m_sharedFenceSecondary.Get(), ++m_sharedFenceValue);

            // Destination half: GPU 0 waits for the source half, then reads the cross-adapter buffer.
            m_primary.queue->Wait(m_sharedFencePrimary.Get(), m_sharedFenceValue);
            ID3D12GraphicsCommandList* primaryList = m_primary.begin(m_frame);
            {
                NAU_GPU_TRANSFER_DST("transfer_test", SecondaryGpu, PrimaryGpu);
                primaryList->CopyBufferRegion(m_transferDst.Get(), 0, m_sharedPrimary.Get(), 0, TransferBufferSize);
            }
            m_primary.submit(m_frame);
            m_primary.queue->Signal(m_sharedFencePrimary.Get(), ++m_sharedFenceValue);
        }
        ++m_frame;
    }

    void SelfTest::shutdown()
    {
        release();
        m_initialized = false;
        m_framesBeforeInit = 0;
    }

    void SelfTest::release()
    {
        // The profiler drops whatever is unfinished; the GPUs must be idle before the timestamp sources go away.
        if (m_primary.queue && m_primary.fence)
        {
            m_primary.waitIdle();
        }
        if (m_secondary.queue && m_secondary.fence)
        {
            m_secondary.waitIdle();
        }
        GpuProfiler& profiler = GpuProfiler::instance();
        if (m_active)
        {
            profiler.unregisterGpu(SecondaryGpu);
        }
        if (profiler.hasGpu(PrimaryGpu) && m_primary.timestamps.isValid())
        {
            profiler.unregisterGpu(PrimaryGpu);
        }

        m_transferDst.Reset();
        m_sharedPrimary.Reset();
        m_sharedSecondary.Reset();
        m_sharedHeapPrimary.Reset();
        m_sharedHeapSecondary.Reset();
        m_sharedFencePrimary.Reset();
        m_sharedFenceSecondary.Reset();
        m_sharedFenceValue = 0;
        m_primary.shutdown();

        m_secondary.shutdown();
        m_copySrc.Reset();
        m_copyDst.Reset();
        m_active = false;
        m_frame = 0;
        m_mode = Mode::Copy;
    }
}  // namespace nau::gpu_profiler
