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
            auto* unknown = static_cast<IUnknown*>(d3d::get_device());
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

        Microsoft::WRL::ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

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

        logDebugMessages();
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
        if (!env || std::atoi(env) == 0 || !GpuProfiler::instance().isEnabled())
        {
            return;
        }

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
    }

    void SelfTest::tick()
    {
        if (!m_active)
        {
            return;
        }

        ID3D12GraphicsCommandList* list = m_secondary.begin(m_frame);
        {
            NAU_GPU_TIMER_ON(SecondaryGpu, "copy_test");
            list->CopyBufferRegion(m_copyDst.Get(), 0, m_copySrc.Get(), 0, CopyBufferSize);
        }
        m_secondary.submit(m_frame);
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
        if (m_active)
        {
            // The profiler drops whatever is unfinished; the GPU must be idle before the timestamp source goes away.
            m_secondary.waitIdle();
            GpuProfiler::instance().unregisterGpu(SecondaryGpu);
        }
        m_secondary.shutdown();
        m_copySrc.Reset();
        m_copyDst.Reset();
        m_active = false;
        m_frame = 0;
    }
}  // namespace nau::gpu_profiler
