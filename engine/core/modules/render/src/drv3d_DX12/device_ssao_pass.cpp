// device_ssao_pass.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "device_ssao_pass.h"
#include "ssao_unpack_depth_bytecode.h"
#include "mailbox_receive.h"
#include "nau/3d/ssao_random_texture.h"
#include "nau/diag/logging.h"

#include <EASTL/vector.h>

#include <cstring>

namespace drv3d_dx12
{
    namespace
    {

        constexpr UINT CONSTANT_BYTES = (sizeof(d3d::mgpu::SsaoConstants) + 255u) & ~255u;
        constexpr UINT MAILBOX_CONTROL_OFFSET = CONSTANT_BYTES;
        enum Timestamp : UINT
        {
            INPUT_BEGIN,
            RAW_BEGIN,
            RAW_END,
            OUTPUT_END,
            TIMESTAMP_COUNT
        };
        constexpr UINT SRV_TABLES = 2, SRVS_PER_TABLE = 5, SRVS_PER_SET = SRV_TABLES * SRVS_PER_TABLE;
        enum XePass : UINT
        {
            XE_PREFILTER,
            XE_MAIN,
            XE_DENOISE,
            XE_DENOISE_LAST,
            XE_COMPOSITE,
            XE_PASS_COUNT
        };
        constexpr UINT XE_SRVS = 5, XE_UAVS = 5, XE_DESCRIPTORS = XE_SRVS + XE_UAVS;

        bool validShader(const D3D12_SHADER_BYTECODE& shader)
        {
            return shader.pShaderBytecode && shader.BytecodeLength != 0;
        }
    }  // namespace

    class DeviceSsaoPass::State
    {
        friend class DeviceSsaoPass;

    public:
        State() = default;
        State(const State&) = delete;
        State(State&&) = delete;
        State& operator=(const State&) = delete;
        State& operator=(State&&) = delete;
        ~State()
        {
            if (m_event)
            {
                CloseHandle(m_event);
            }
        }

        HRESULT init(const DeviceSsaoPassDesc& desc);
        HRESULT execute(const d3d::mgpu::SsaoConstants& data, const MailboxSsaoTextures& shared, const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion);
        void pollTiming();
        HRESULT wait(DWORD timeoutMs);
        bool removed() const
        {
            return m_device && FAILED(m_device->GetDeviceRemovedReason());
        }

    private:
        HRESULT createDescriptors();
        HRESULT createRandomTexture();
        HRESULT createPipelines(const DeviceSsaoPassDesc& desc);
        HRESULT signal();
        void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
        struct Recording
        {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> publication;
            DeviceResource constants;
            uint8_t* mapped = nullptr;
            UINT64 fenceValue = 0;
            Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap;
            DeviceResource timestampReadback;
            uint64_t* timestamps = nullptr;
            uint64_t timingFrame = 0;
            bool timingPending = false;

            ~Recording()
            {
                if (timestamps)
                {
                    timestampReadback.getResource()->Unmap(0, nullptr);
                }
                if (mapped)
                {
                    constants.getResource()->Unmap(0, nullptr);
                }
            }
        };

        HRESULT acquireRecording(Recording*& recording);
        HRESULT createXeDescriptors();
        HRESULT createXePipelines(const DeviceSsaoPassDesc& desc);
        void executeXe(const d3d::mgpu::SsaoConstants& data, Recording& recording);

    private:
        // Declaration order keeps the device and allocator alive until the resources are destroyed.
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        eastl::shared_ptr<IDeviceResourceAllocator> m_allocator;
        eastl::shared_ptr<void> m_mailboxOwner;
        struct Resources
        {
            DeviceResource normals;
            DeviceResource depth;
            DeviceResource output;
            DeviceResource xeDepth;
            DeviceResource xePing;
            DeviceResource xePong;
            DeviceResource xeEdges;
            bool targetsInitialized = false;
        };
        Resources m_resources;
        DXGI_FORMAT m_normalFormat = DXGI_FORMAT_R16G16_FLOAT;
        DXGI_FORMAT m_depthFormat = DXGI_FORMAT_R32_FLOAT;
        DeviceResource m_random;
        DeviceResource m_randomUpload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_randomFootprint = {};
        bool m_randomUploaded = false;
        bool m_reconstructNormals = false;
        bool m_computeRaw = false;
        bool m_xegtao = false;
        bool m_packedFrameworkDepth = false;
        bool m_packedFrameworkNormals = false;
        DeviceResource m_packedDepth;
        DeviceResource m_packedNormals;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_unpackRtvHeap;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_unpackPso;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        eastl::vector<eastl::unique_ptr<Recording>> m_recordings;
        ID3D12GraphicsCommandList* m_list = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRoot;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_rawPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_computeRawPso;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_xeHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_xeRoot;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_xePso[XE_PASS_COUNT];
        HANDLE m_event = nullptr;
        UINT m_width = 0;
        UINT m_height = 0;
        UINT m_srvStride = 0;
        UINT m_rtvStride = 0;
        UINT64 m_serial = 0;
        bool m_signalPending = false;
        UINT64 m_timestampFrequency = 0;
        d3d::mgpu::SsaoGpuTiming m_timing;
        State* m_nextRetired = nullptr;
    };

    DeviceSsaoPass::State* DeviceSsaoPass::s_retired = nullptr;
    DeviceSsaoPass::DeviceSsaoPass() = default;

    DeviceSsaoPass::~DeviceSsaoPass()
    {
        reset(0);
        if (m_state)
        {
            m_state->m_nextRetired = s_retired;
            s_retired = m_state.release();
            OutputDebugStringA("SSAO: retired pending GPU 1 resources.\n");
        }
    }

    HRESULT DeviceSsaoPass::init(const DeviceSsaoPassDesc& desc)
    {
        collectRetiredResources();
        if (m_state)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        if (!desc.allocator || !desc.allocator->getDevice() || desc.width == 0 || desc.height == 0 ||
            desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.inputWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.inputHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            !validShader(desc.vertexShader) || !validShader(desc.rawShader))
        {
            return E_INVALIDARG;
        }

        eastl::unique_ptr<State> state(new State());
        const HRESULT result = state->init(desc);
        if (SUCCEEDED(result))
        {
            m_state = eastl::move(state);
        }
        return result;
    }

    HRESULT DeviceSsaoPass::reset(DWORD timeoutMs)
    {
        if (!m_state)
        {
            return S_OK;
        }
        const HRESULT result = m_state->wait(timeoutMs);
        if (SUCCEEDED(result) || m_state->removed())
        {
            m_state.reset();
        }
        return result;
    }

    size_t DeviceSsaoPass::collectRetiredResources()
    {
        size_t pending = 0;
        State** current = &s_retired;
        while (*current)
        {
            State* state = *current;
            if (SUCCEEDED(state->wait(0)) || state->removed())
            {
                *current = state->m_nextRetired;
                delete state;
            }
            else
            {
                ++pending;
                current = &state->m_nextRetired;
            }
        }
        return pending;
    }

    d3d::mgpu::SsaoGpuTiming DeviceSsaoPass::pollTiming()
    {
        if (!m_state)
        {
            return {};
        }
        m_state->pollTiming();
        return m_state->m_timing;
    }

    HRESULT DeviceSsaoPass::executeMailbox(const d3d::mgpu::SsaoConstants& constants, const MailboxSsaoTextures& shared, const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion)
    {
        completion = {};
        if (!m_state || !shared.depth || !shared.result ||
            (!m_state->m_reconstructNormals && !shared.normals) || !job.completion || !job.frameId ||
            !job.inputsReady.fence || !job.inputsReady.value || job.outputSlot >= MAILBOX_OUT_SLOTS ||
            !job.mailbox || !frame || frame > UINT32_MAX)
        {
            return E_INVALIDARG;
        }
        return m_state->execute(constants, shared, job, frame, completion);
    }

    void DeviceSsaoPass::retainMailboxResources(const eastl::shared_ptr<void>& resources)
    {
        if (m_state)
        {
            m_state->m_mailboxOwner = resources;
        }
    }

    HRESULT DeviceSsaoPass::State::init(const DeviceSsaoPassDesc& desc)
    {
        m_device = desc.allocator->getDevice();
        m_allocator = desc.allocator;
        m_reconstructNormals = desc.reconstructNormals;
        m_packedFrameworkDepth = desc.packedFrameworkDepth;
        m_packedFrameworkNormals = m_packedFrameworkDepth && desc.packedFrameworkNormals && !m_reconstructNormals;
        m_timing.packedDepth = m_packedFrameworkDepth;
        m_timing.packedNormals = m_packedFrameworkNormals;
        m_width = desc.width;
        m_height = desc.height;
        m_normalFormat = desc.packedNormals ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_FLOAT;
        m_depthFormat = desc.linearDepth16 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        m_xegtao = validShader(desc.xePrefilterShader) || validShader(desc.xeMainShader) ||
                   validShader(desc.xeDenoiseShader) || validShader(desc.xeDenoiseLastShader) || validShader(desc.xeCompositeShader);
        if (m_xegtao)
        {
            if (m_width < 16 || m_height < 16)
            {
                return E_INVALIDARG;
            }
            if (!validShader(desc.xePrefilterShader) || !validShader(desc.xeMainShader) || !validShader(desc.xeDenoiseShader) ||
                !validShader(desc.xeDenoiseLastShader) || !validShader(desc.xeCompositeShader))
            {
                return E_INVALIDARG;
            }
            for (DXGI_FORMAT format : {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8_UNORM})
            {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
                const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                      D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW |
                                      (format != DXGI_FORMAT_R32_UINT ? D3D12_FORMAT_SUPPORT1_SHADER_GATHER : D3D12_FORMAT_SUPPORT1_NONE) |
                                      (format == DXGI_FORMAT_R16_FLOAT ? D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE : D3D12_FORMAT_SUPPORT1_NONE);
                const HRESULT formatResult = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
                if (FAILED(formatResult) ||
                    (support.Support1 & required) != required || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
                {
                    NAU_LOG_ERROR("XeGTAO: secondary format {} unsupported, HRESULT={:#x}, Support1={:#x}, required1={:#x}, Support2={:#x}, required2={:#x}",
                                  static_cast<uint32_t>(format), static_cast<uint32_t>(formatResult), static_cast<uint32_t>(support.Support1),
                                  static_cast<uint32_t>(required), static_cast<uint32_t>(support.Support2), static_cast<uint32_t>(D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
                    return DXGI_ERROR_UNSUPPORTED;
                }
            }
            m_computeRaw = true;
        }
        else if (validShader(desc.rawComputeShader))
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {DXGI_FORMAT_R8_UNORM};
            const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
            m_computeRaw = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                           (support.Support1 & required) == required && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            // The same algorithm is present in rawShader. Unsupported adapters keep the pixel path.
            if (!m_computeRaw)
            {
                NAU_LOG_WARNING("AO: secondary R8 typed UAV stores unavailable; using the same algorithm's pixel path");
            }
        }
        const auto aoFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                             (m_computeRaw ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT result = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
        if (SUCCEEDED(result) && desc.measureTimings)
        {
            result = m_queue->GetTimestampFrequency(&m_timestampFrequency);
        }
        if (SUCCEEDED(result))
        {
            result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        }
        if (SUCCEEDED(result))
        {
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!m_event)
            {
                result = HRESULT_FROM_WIN32(GetLastError());
            }
        }
        auto& resources = m_resources;
        if (SUCCEEDED(result))
        {
            result = resources.normals.createTexture(m_allocator.get(), m_reconstructNormals ? 1u : (desc.inputWidth ? desc.inputWidth : m_width),
                                                     m_reconstructNormals ? 1u : (desc.inputHeight ? desc.inputHeight : m_height), m_normalFormat,
                                                     m_packedFrameworkNormals ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
        }
        if (SUCCEEDED(result))
        {
            result = resources.depth.createTexture(m_allocator.get(), desc.inputWidth ? desc.inputWidth : m_width,
                                                   desc.inputHeight ? desc.inputHeight : m_height, m_depthFormat,
                                                   m_packedFrameworkDepth ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
        }
        if (SUCCEEDED(result))
        {
            result = resources.output.createTexture(m_allocator.get(), m_width, m_height, DXGI_FORMAT_R8_UNORM, aoFlags);
        }
        if (SUCCEEDED(result) && m_xegtao)
        {
            result = resources.xeDepth.createTexture(m_allocator.get(), m_width,
                                                     m_height, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 5);
            if (SUCCEEDED(result))
            {
                result = resources.xePing.createTexture(m_allocator.get(), m_width, m_height,
                                                        DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            if (SUCCEEDED(result))
            {
                result = resources.xePong.createTexture(m_allocator.get(), m_width, m_height,
                                                        DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            if (SUCCEEDED(result))
            {
                result = resources.xeEdges.createTexture(m_allocator.get(), m_width, m_height,
                                                         DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
        }
        if (SUCCEEDED(result) && m_packedFrameworkDepth)
        {
            const UINT inputWidth = desc.inputWidth ? desc.inputWidth : m_width;
            const UINT pixelsPerWord = desc.linearDepth16 ? 8u : 4u;
            result = m_packedDepth.createTexture(m_allocator.get(), (inputWidth + pixelsPerWord - 1) / pixelsPerWord,
                                                 desc.inputHeight ? desc.inputHeight : m_height, DXGI_FORMAT_R32G32B32A32_UINT);
            if (SUCCEEDED(result) && m_packedFrameworkNormals)
            {
                const UINT normalGroup = desc.packedNormals ? 8u : 4u;
                result = m_packedNormals.createTexture(m_allocator.get(), (inputWidth + normalGroup - 1) / normalGroup,
                                                       desc.inputHeight ? desc.inputHeight : m_height, DXGI_FORMAT_R32G32B32A32_UINT);
            }
        }
        if (SUCCEEDED(result))
        {
            result = createRandomTexture();
        }
        if (SUCCEEDED(result))
        {
            result = createDescriptors();
        }
        if (SUCCEEDED(result))
        {
            result = createPipelines(desc);
        }
        if (SUCCEEDED(result) && m_xegtao)
        {
            result = createXeDescriptors();
        }
        if (SUCCEEDED(result) && m_xegtao)
        {
            result = createXePipelines(desc);
        }
        if (SUCCEEDED(result) && m_xegtao)
        {
            NAU_LOG_INFO("XeGTAO: GPU 1 raw AO path=compute, quality=high, denoise=2");
        }
        if (SUCCEEDED(result) && !m_xegtao && validShader(desc.rawComputeShader))
        {
            NAU_LOG_INFO("HBAO: GPU 1 raw AO path={}", m_computeRaw ? "compute" : "pixel");
        }

        return result;
    }

    HRESULT DeviceSsaoPass::State::createRandomTexture()
    {
        constexpr UINT SIZE = d3d::mgpu::SSAO_RANDOM_TEXTURE_SIZE;
        HRESULT result = m_random.createTexture(m_allocator.get(), SIZE, SIZE, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (FAILED(result))
        {
            return result;
        }
        const D3D12_RESOURCE_DESC desc = m_random.getResource()->GetDesc();
        UINT64 uploadBytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &m_randomFootprint, nullptr, nullptr, &uploadBytes);
        result = m_randomUpload.createBuffer(m_allocator.get(), uploadBytes, D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(result))
        {
            return result;
        }
        const D3D12_RANGE noRead = {0, 0};
        void* mapped = nullptr;
        result = m_randomUpload.getResource()->Map(0, &noRead, &mapped);
        if (SUCCEEDED(result))
        {
            for (UINT y = 0; y < SIZE; ++y)
            {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + m_randomFootprint.Offset + y * m_randomFootprint.Footprint.RowPitch);
                for (UINT x = 0; x < SIZE; ++x)
                {
                    row[x] = d3d::mgpu::ssaoRandomTexel(y * SIZE + x);
                }
            }
            m_randomUpload.getResource()->Unmap(0, nullptr);
        }
        return result;
    }

    HRESULT DeviceSsaoPass::State::createDescriptors()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        HRESULT result = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = SRVS_PER_SET + (m_computeRaw ? 1u : 0u);
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (SUCCEEDED(result))
        {
            result = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap));
        }
        if (FAILED(result))
        {
            return result;
        }
        m_srvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (m_packedFrameworkDepth)
        {
            D3D12_DESCRIPTOR_HEAP_DESC unpack = {};
            unpack.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            unpack.NumDescriptors = m_packedFrameworkNormals ? 2u : 1u;
            result = m_device->CreateDescriptorHeap(&unpack, IID_PPV_ARGS(&m_unpackRtvHeap));
            if (FAILED(result))
            {
                return result;
            }
            m_device->CreateRenderTargetView(m_resources.depth.getResource(), nullptr, m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart());
            if (m_packedFrameworkNormals)
            {
                auto handle = m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += m_rtvStride;
                m_device->CreateRenderTargetView(m_resources.normals.getResource(), nullptr, handle);
            }
        }
        auto& resources = m_resources;
        m_device->CreateRenderTargetView(resources.output.getResource(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT table = 0; table < SRV_TABLES; ++table)
        {
            ID3D12Resource* inputs[] = {table == 1 && m_packedFrameworkNormals ? m_packedNormals.getResource() : resources.normals.getResource(), resources.depth.getResource(),
                                        nullptr,
                                        table == 1 && m_packedFrameworkDepth ? m_packedDepth.getResource() : resources.depth.getResource(), m_random.getResource()};
            for (UINT input = 0; input < 5; ++input)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = inputs[input] ? inputs[input]->GetDesc().Format : DXGI_FORMAT_R8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (table * SRVS_PER_TABLE + input) * m_srvStride;
                m_device->CreateShaderResourceView(inputs[input], &srv, handle);
            }
        }
        if (m_computeRaw)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R8_UNORM;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SRVS_PER_SET * m_srvStride;
            m_device->CreateUnorderedAccessView(resources.output.getResource(), nullptr, &uav, handle);
        }
        return S_OK;
    }

    HRESULT DeviceSsaoPass::State::createXeDescriptors()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = XE_PASS_COUNT * XE_DESCRIPTORS;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT result = m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_xeHeap));
        if (FAILED(result))
        {
            return result;
        }
        auto& resources = m_resources;
        for (UINT pass = 0; pass < XE_PASS_COUNT; ++pass)
        {
            ID3D12Resource* sources[] = {resources.xeDepth.getResource(), resources.xeDepth.getResource(),
                                         resources.xePing.getResource(), resources.xePong.getResource(), resources.xePing.getResource()};
            ID3D12Resource* inputs[] = {resources.normals.getResource(), resources.depth.getResource(),
                                        sources[pass], resources.xeEdges.getResource(), m_random.getResource()};
            const UINT base = pass * XE_DESCRIPTORS;
            for (UINT input = 0; input < XE_SRVS; ++input)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = inputs[input] ? inputs[input]->GetDesc().Format : DXGI_FORMAT_R8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = inputs[input]->GetDesc().MipLevels;
                auto handle = m_xeHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (base + input) * m_srvStride;
                m_device->CreateShaderResourceView(inputs[input], &srv, handle);
            }
            ID3D12Resource* targets[] = {resources.xeDepth.getResource(), resources.xePing.getResource(),
                                         resources.xePong.getResource(), resources.xePing.getResource(), resources.output.getResource()};
            for (UINT output = 0; output < XE_UAVS; ++output)
            {
                ID3D12Resource* destination = pass == XE_MAIN && output == 1 ? resources.xeEdges.getResource() : targets[pass];
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
                uav.Format = destination->GetDesc().Format;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D.MipSlice = pass == XE_PREFILTER ? output : 0;
                auto handle = m_xeHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (base + XE_SRVS + output) * m_srvStride;
                m_device->CreateUnorderedAccessView(destination, nullptr, &uav, handle);
            }
        }
        return S_OK;
    }

    HRESULT DeviceSsaoPass::State::createXePipelines(const DeviceSsaoPassDesc& desc)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = XE_SRVS;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = XE_UAVS;
        D3D12_ROOT_PARAMETER parameters[3] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (UINT index = 0; index < 2; ++index)
        {
            parameters[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[index + 1].DescriptorTable.NumDescriptorRanges = 1;
            parameters[index + 1].DescriptorTable.pDescriptorRanges = &ranges[index];
        }
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for (UINT index = 0; index < 2; ++index)
        {
            samplers[index].Filter = index == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[index].AddressU = samplers[index].AddressV = samplers[index].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[index].MaxAnisotropy = 1;
            samplers[index].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[index].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[index].ShaderRegister = index;
        }
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 3;
        root.pParameters = parameters;
        root.NumStaticSamplers = 2;
        root.pStaticSamplers = samplers;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        HRESULT result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors);
        if (SUCCEEDED(result))
        {
            result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_xeRoot));
        }
        const D3D12_SHADER_BYTECODE shaders[] = {desc.xePrefilterShader, desc.xeMainShader,
                                                 desc.xeDenoiseShader, desc.xeDenoiseLastShader, desc.xeCompositeShader};
        for (UINT pass = 0; pass < XE_PASS_COUNT && SUCCEEDED(result); ++pass)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
            pipeline.pRootSignature = m_xeRoot.Get();
            pipeline.CS = shaders[pass];
            result = m_device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_xePso[pass]));
        }
        return result;
    }

    HRESULT DeviceSsaoPass::State::createPipelines(const DeviceSsaoPassDesc& desc)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 5;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = parameters;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for (UINT index = 0; index < 2; ++index)
        {
            samplers[index].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[index].AddressU = samplers[index].AddressV = samplers[index].AddressW =
                index == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[index].MaxAnisotropy = 1;
            samplers[index].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[index].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[index].ShaderRegister = index;
            samplers[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        rootDesc.NumStaticSamplers = 2;
        rootDesc.pStaticSamplers = samplers;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors);
        if (SUCCEEDED(result))
        {
            result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_root));
        }
        if (FAILED(result))
        {
            return result;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_root.Get();
        pso.VS = desc.vertexShader;
        pso.PS = desc.rawShader;
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace =
            {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        pso.SampleDesc.Count = 1;
        result = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_rawPso));
        if (SUCCEEDED(result) && m_packedFrameworkDepth)
        {
            pso.VS = {g_ssaoUnpackDepthVertexBytecode, sizeof(g_ssaoUnpackDepthVertexBytecode)};
            pso.PS = {g_ssaoUnpackDepthBytecode, sizeof(g_ssaoUnpackDepthBytecode)};
            if (m_packedFrameworkNormals)
            {
                pso.PS = {g_ssaoUnpackDepthNormalsBytecode, sizeof(g_ssaoUnpackDepthNormalsBytecode)};
                pso.NumRenderTargets = 2;
                pso.RTVFormats[1] = m_normalFormat;
            }
            pso.RTVFormats[0] = m_depthFormat;
            result = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_unpackPso));
        }
        if (SUCCEEDED(result) && m_computeRaw && !m_xegtao)
        {
            D3D12_DESCRIPTOR_RANGE ranges[2] = {};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = SRVS_PER_TABLE;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[1].NumDescriptors = 1;
            D3D12_ROOT_PARAMETER computeParameters[3] = {};
            computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            computeParameters[1].DescriptorTable.NumDescriptorRanges = 1;
            computeParameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
            computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            computeParameters[2].DescriptorTable.NumDescriptorRanges = 1;
            computeParameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
            for (auto& parameter : computeParameters)
            {
                parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            }
            for (auto& sampler : samplers)
            {
                sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            }
            D3D12_ROOT_SIGNATURE_DESC computeRoot = {};
            computeRoot.NumParameters = 3;
            computeRoot.pParameters = computeParameters;
            computeRoot.NumStaticSamplers = 2;
            computeRoot.pStaticSamplers = samplers;
            serialized.Reset();
            errors.Reset();
            result = D3D12SerializeRootSignature(&computeRoot, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors);
            if (SUCCEEDED(result))
            {
                result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_computeRoot));
            }
            if (SUCCEEDED(result))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC computePso = {};
                computePso.pRootSignature = m_computeRoot.Get();
                computePso.CS = desc.rawComputeShader;
                result = m_device->CreateComputePipelineState(&computePso, IID_PPV_ARGS(&m_computeRawPso));
            }
        }
        return result;
    }

    HRESULT DeviceSsaoPass::State::signal()
    {
        const HRESULT result = m_queue->Signal(m_fence.Get(), m_serial);
        m_signalPending = FAILED(result);
        return result;
    }

    HRESULT DeviceSsaoPass::State::acquireRecording(Recording*& recording)
    {
        pollTiming();
        if (m_signalPending || m_serial >= UINT64_MAX - 4)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        const UINT64 completed = m_fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        for (const eastl::unique_ptr<Recording>& candidate : m_recordings)
        {
            if (candidate->fenceValue <= completed)
            {
                recording = candidate.get();
                HRESULT result = recording->allocator->Reset();
                return SUCCEEDED(result) ? recording->list->Reset(recording->allocator.Get(), m_rawPso.Get()) : result;
            }
        }
        if (m_recordings.size() >= MAILBOX_IN_SLOTS)
        {
            ++m_timing.skippedSubmissions;
            return S_FALSE;
        }
        eastl::unique_ptr<Recording> next = eastl::make_unique<Recording>();
        HRESULT result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next->allocator));
        if (SUCCEEDED(result))
        {
            result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next->allocator.Get(), m_rawPso.Get(), IID_PPV_ARGS(&next->list));
        }
        if (SUCCEEDED(result))
        {
            result = next->list->Close();
            if (SUCCEEDED(result))
            {
                result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next->allocator.Get(), nullptr, IID_PPV_ARGS(&next->publication));
            }
            if (SUCCEEDED(result))
            {
                result = next->publication->Close();
            }
            if (SUCCEEDED(result))
            {
                result = next->list->Reset(next->allocator.Get(), m_rawPso.Get());
            }
        }
        if (SUCCEEDED(result))
        {
            result = next->constants.createBuffer(m_allocator.get(), MAILBOX_CONTROL_OFFSET + 512u, D3D12_HEAP_TYPE_UPLOAD);
        }
        if (SUCCEEDED(result))
        {
            const D3D12_RANGE noRead = {0, 0};
            void* mapped = nullptr;
            result = next->constants.getResource()->Map(0, &noRead, &mapped);
            next->mapped = static_cast<uint8_t*>(mapped);
        }
        if (SUCCEEDED(result))
        {
            if (m_timestampFrequency)
            {
                const D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, TIMESTAMP_COUNT, 0};
                result = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&next->timestampHeap));
                if (SUCCEEDED(result))
                {
                    result = next->timestampReadback.createBuffer(m_allocator.get(), TIMESTAMP_COUNT * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
                }
                if (SUCCEEDED(result))
                {
                    const D3D12_RANGE read = {0, TIMESTAMP_COUNT * sizeof(uint64_t)};
                    void* mapped = nullptr;
                    result = next->timestampReadback.getResource()->Map(0, &read, &mapped);
                    next->timestamps = static_cast<uint64_t*>(mapped);
                }
            }
        }
        if (SUCCEEDED(result))
        {
            recording = next.get();
            m_recordings.push_back(eastl::move(next));
        }
        return result;
    }

    void DeviceSsaoPass::State::pollTiming()
    {
        if (!m_fence)
        {
            return;
        }
        const uint64_t completed = m_fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            return;
        }
        for (auto& recording : m_recordings)
        {
            if (!recording->timingPending || recording->fenceValue > completed)
            {
                continue;
            }
            ++m_timing.completedJobs;
            const auto* t = recording->timestamps;
            if (m_timestampFrequency && t && recording->timingFrame > m_timing.frame &&
                t[RAW_BEGIN] >= t[INPUT_BEGIN] && t[RAW_END] >= t[RAW_BEGIN] &&
                t[OUTPUT_END] >= t[RAW_END])
            {
                const auto ms = [&](Timestamp first, Timestamp last)
                {
                    return static_cast<float>(1000.0 * static_cast<double>(t[last] - t[first]) / m_timestampFrequency);
                };
                m_timing.frame = recording->timingFrame;
                m_timing.rawMilliseconds = ms(RAW_BEGIN, RAW_END);
                m_timing.totalMilliseconds = m_timing.rawMilliseconds;
                m_timing.inputMilliseconds = ms(INPUT_BEGIN, RAW_BEGIN);
                m_timing.outputMilliseconds = ms(RAW_END, OUTPUT_END);
                m_timing.jobMilliseconds = ms(INPUT_BEGIN, OUTPUT_END);
            }
            recording->timingPending = false;
        }
    }

    HRESULT DeviceSsaoPass::State::wait(DWORD timeoutMs)
    {
        if (!m_fence)
        {
            return S_OK;
        }
        if (m_signalPending)
        {
            const HRESULT result = signal();
            if (FAILED(result))
            {
                return result;
            }
        }
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            const UINT64 completed = m_fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            if (completed >= m_serial)
            {
                return S_OK;
            }
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            const HRESULT result = m_fence->SetEventOnCompletion(m_serial, m_event);
            if (FAILED(result))
            {
                return result;
            }
            const DWORD status = WaitForSingleObject(m_event, timeoutMs - static_cast<DWORD>(elapsed));
            if (status == WAIT_FAILED)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
    }

    void DeviceSsaoPass::State::barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER transition = {};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        m_list->ResourceBarrier(1, &transition);
    }

    void DeviceSsaoPass::State::executeXe(const d3d::mgpu::SsaoConstants& data, Recording& recording)
    {
        auto& resources = m_resources;
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12DescriptorHeap* heap[] = {m_xeHeap.Get()};
        m_list->SetDescriptorHeaps(1, heap);
        m_list->SetComputeRootSignature(m_xeRoot.Get());
        std::memcpy(recording.mapped, &data, sizeof(data));
        m_list->SetComputeRootConstantBufferView(0, recording.constants.getResource()->GetGPUVirtualAddress());
        const auto dispatch = [&](XePass pass, UINT groupsX, UINT groupsY)
        {
            auto srv = m_xeHeap->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += pass * XE_DESCRIPTORS * m_srvStride;
            auto uav = srv;
            uav.ptr += XE_SRVS * m_srvStride;
            m_list->SetComputeRootDescriptorTable(1, srv);
            m_list->SetComputeRootDescriptorTable(2, uav);
            m_list->SetPipelineState(m_xePso[pass].Get());
            m_list->Dispatch(groupsX, groupsY, 1);
        };
        barrier(resources.xeDepth.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_PREFILTER, (m_width + 15) / 16, (m_height + 15) / 16);
        barrier(resources.xeDepth.getResource(), write, read);
        barrier(resources.xePing.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        barrier(resources.xeEdges.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_MAIN, (m_width + 7) / 8, (m_height + 7) / 8);
        barrier(resources.xePing.getResource(), write, read);
        barrier(resources.xeEdges.getResource(), write, read);
        barrier(resources.xePong.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_DENOISE, (m_width + 15) / 16, (m_height + 7) / 8);
        barrier(resources.xePong.getResource(), write, read);
        barrier(resources.xePing.getResource(), read, write);
        dispatch(XE_DENOISE_LAST, (m_width + 15) / 16, (m_height + 7) / 8);
        barrier(resources.xePing.getResource(), write, read);
        auto* target = resources.output.getResource();
        barrier(target, D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_COMPOSITE, (m_width + 7) / 8, (m_height + 7) / 8);
        barrier(target, write, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.xeDepth.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.xePing.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.xePong.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.xeEdges.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
    }

    HRESULT DeviceSsaoPass::State::execute(const d3d::mgpu::SsaoConstants& data, const MailboxSsaoTextures& shared, const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion)
    {
        auto& resources = m_resources;
        Recording* recording = nullptr;
        HRESULT result = acquireRecording(recording);
        if (result != S_OK)
        {
            return result;
        }
        m_list = recording->list.Get();
        const auto copyShared = [&](ID3D12Resource* local, ID3D12Resource* common, bool toShared)
        {
            const auto localState = toShared ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
            const auto sharedState = toShared ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier(local, D3D12_RESOURCE_STATE_COMMON, localState);
            barrier(common, D3D12_RESOURCE_STATE_COMMON, sharedState);
            m_list->CopyResource(toShared ? common : local, toShared ? local : common);
            barrier(common, sharedState, D3D12_RESOURCE_STATE_COMMON);
            barrier(local, localState, D3D12_RESOURCE_STATE_COMMON);
        };
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, INPUT_BEGIN);
        }
        if (!m_reconstructNormals)
        {
            copyShared(m_packedFrameworkNormals ? m_packedNormals.getResource() : resources.normals.getResource(), shared.normals, false);
        }
        copyShared(m_packedFrameworkDepth ? m_packedDepth.getResource() : resources.depth.getResource(), shared.depth, false);
        if (!m_randomUploaded)
        {
            barrier(m_random.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = m_randomUpload.getResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = m_randomFootprint;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = m_random.getResource();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            barrier(m_random.getResource(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootSignature(m_root.Get());
        if (m_packedFrameworkDepth)
        {
            // Expand the wide shared-memory view into the original local depth format before SSAO.
            // GPU0 still writes its ordinary R16F/R32F texture; no packing pass or frame flush is needed there.
            const auto depthDesc = resources.depth.getResource()->GetDesc();
            const D3D12_VIEWPORT unpackViewport = {0, 0, static_cast<float>(depthDesc.Width), static_cast<float>(depthDesc.Height), 0, 1};
            const D3D12_RECT unpackScissor = {0, 0, static_cast<LONG>(depthDesc.Width), static_cast<LONG>(depthDesc.Height)};
            m_list->RSSetViewports(1, &unpackViewport);
            m_list->RSSetScissorRects(1, &unpackScissor);
            m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            std::memcpy(recording->mapped, &data, sizeof(data));
            m_list->SetGraphicsRootConstantBufferView(0, recording->constants.getResource()->GetGPUVirtualAddress());
            auto table = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += SRVS_PER_TABLE * m_srvStride;
            m_list->SetGraphicsRootDescriptorTable(1, table);
            m_list->SetPipelineState(m_unpackPso.Get());
            barrier(m_packedDepth.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(resources.depth.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart(), m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart()};
            targets[1].ptr += m_rtvStride;
            if (m_packedFrameworkNormals)
            {
                barrier(m_packedNormals.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                barrier(resources.normals.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            m_list->OMSetRenderTargets(m_packedFrameworkNormals ? 2u : 1u, targets, FALSE, nullptr);
            m_list->DrawInstanced(3, 1, 0, 0);
            barrier(resources.depth.getResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            barrier(m_packedDepth.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (m_packedFrameworkNormals)
            {
                barrier(resources.normals.getResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                barrier(m_packedNormals.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
        }
        const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        m_list->RSSetViewports(1, &viewport);
        m_list->RSSetScissorRects(1, &scissor);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto inputState = m_computeRaw ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier(resources.depth.getResource(), D3D12_RESOURCE_STATE_COMMON, inputState);
        barrier(resources.normals.getResource(), D3D12_RESOURCE_STATE_COMMON, inputState);
        if (m_computeRaw && !resources.targetsInitialized)
        {
            // Initialize placed render-target metadata before using the output as a UAV.
            constexpr float clear[4] = {};
            auto* target = resources.output.getResource();
            barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_list->ClearRenderTargetView(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
            barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            resources.targetsInitialized = true;
        }
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RAW_BEGIN);
        }
        if (m_xegtao)
        {
            executeXe(data, *recording);
        }
        else if (m_computeRaw)
        {
            auto* target = resources.output.getResource();
            std::memcpy(recording->mapped, &data, sizeof(data));
            m_list->SetComputeRootSignature(m_computeRoot.Get());
            m_list->SetComputeRootConstantBufferView(0, recording->constants.getResource()->GetGPUVirtualAddress());
            auto srv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            m_list->SetComputeRootDescriptorTable(1, srv);
            auto uav = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            uav.ptr += SRVS_PER_SET * m_srvStride;
            m_list->SetComputeRootDescriptorTable(2, uav);
            m_list->SetPipelineState(m_computeRawPso.Get());
            barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_list->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
            barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            auto* target = resources.output.getResource();
            std::memcpy(recording->mapped, &data, sizeof(data));
            m_list->SetGraphicsRootConstantBufferView(0, recording->constants.getResource()->GetGPUVirtualAddress());
            m_list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
            m_list->SetPipelineState(m_rawPso.Get());
            barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            m_list->DrawInstanced(3, 1, 0, 0);
            barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RAW_END);
        }
        barrier(resources.normals.getResource(), inputState, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.depth.getResource(), inputState, D3D12_RESOURCE_STATE_COMMON);
        if (recording->timestampHeap)
        {
            recording->timingFrame = frame;
        }
        copyShared(resources.output.getResource(), shared.result, true);
        {
            const uint32_t id = static_cast<uint32_t>(frame);
            std::memcpy(recording->mapped + MAILBOX_CONTROL_OFFSET, &id, sizeof(id));
            D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
            source.pResource = recording->constants.getResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset = MAILBOX_CONTROL_OFFSET;
            source.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
            destination.pResource = job.frameId;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            barrier(job.frameId, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            barrier(job.frameId, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, OUTPUT_END);
            m_list->ResolveQueryData(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, TIMESTAMP_COUNT, recording->timestampReadback.getResource(), 0);
        }

        result = m_list->Close();
        if (FAILED(result))
        {
            return result;
        }
        // Record publication before submitting the AO command list.
        result = recording->publication->Reset(recording->allocator.Get(), nullptr);
        if (FAILED(result))
        {
            return result;
        }
        uint64_t entries[MAILBOX_OUT_SLOTS] = {};
        entries[job.outputSlot] = frame;
        std::memcpy(recording->mapped + MAILBOX_CONTROL_OFFSET + 256, entries, sizeof(entries));
        D3D12_RESOURCE_BARRIER transition = {};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition = {job.mailbox, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        recording->publication->ResourceBarrier(1, &transition);
        recording->publication->CopyBufferRegion(job.mailbox, 0, recording->constants.getResource(), MAILBOX_CONTROL_OFFSET + 256, sizeof(entries));
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        recording->publication->ResourceBarrier(1, &transition);
        result = recording->publication->Close();
        if (FAILED(result))
        {
            return result;
        }
        // Only GPU1 waits. F0 covers these inputs and previous GPU0 readers of the output slot.
        result = m_queue->Wait(job.inputsReady.fence, job.inputsReady.value);
        if (FAILED(result))
        {
            return result;
        }
        ID3D12CommandList* lists[] = {m_list};
        m_queue->ExecuteCommandLists(1, lists);
        m_randomUploaded = true;

        // Reuse the pass's normal completion signal for the queries. Track ownership even if publication fails.
        recording->fenceValue = ++m_serial;
        recording->timingPending = true;
        ++m_timing.submittedJobs;
        completion.dataReady = job.completion->enqueueSignal(m_queue.Get());
        if (!completion.dataReady)
        {
            signal();
            return E_FAIL;
        }
        ID3D12CommandList* publish[] = {recording->publication.Get()};
        m_queue->ExecuteCommandLists(1, publish);
        completion.published = job.completion->enqueueSignal(m_queue.Get());
        if (!completion.published)
        {
            signal();
            return E_FAIL;
        }

        return signal();
    }
}  // namespace drv3d_dx12
