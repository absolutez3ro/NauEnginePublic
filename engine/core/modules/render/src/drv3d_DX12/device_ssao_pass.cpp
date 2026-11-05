// device_ssao_pass.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "device_ssao_pass.h"
#include "ssao_unpack_depth_bytecode.h"
#include "mailbox_receive.h"
#include "nau/3d/ssao_random_texture.h"

#include <EASTL/vector.h>

#include <cstring>

namespace drv3d_dx12
{
    namespace
    {
        constexpr UINT CONSTANT_BYTES = (sizeof(d3d::mgpu::SsaoConstants) + 255u) & ~255u;
        constexpr UINT MAILBOX_CONTROL_OFFSET = CONSTANT_BYTES * 3;
        enum Timestamp : UINT { INPUT_BEGIN, RAW_BEGIN, RAW_END, FILTER_END, OUTPUT_END, TIMESTAMP_COUNT };
        constexpr UINT SRV_TABLES = 4, SRVS_PER_TABLE = 5, SRVS_PER_SET = SRV_TABLES * SRVS_PER_TABLE;

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
        HRESULT execute(const d3d::mgpu::SsaoConstants& data, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady, uint64_t frame, uint32_t resourceIndex, const FrameworkSsaoTextures* shared = nullptr,
            const MailboxSsaoJob* mailbox = nullptr, MailboxSsaoCompletion* completion = nullptr);
        HRESULT releaseOutput(CrossAdapterSyncPoint released, uint32_t resourceIndex);
        void pollTiming();
        HRESULT wait(DWORD timeoutMs);
        HRESULT waitOn(CrossAdapterSyncPoint point, UINT index);
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
                if (timestamps) timestampReadback.getResource()->Unmap(0, nullptr);
                if (mapped)
                {
                    constants.getResource()->Unmap(0, nullptr);
                }
            }
        };

        HRESULT acquireRecording(Recording*& recording);

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
            DeviceResource raw;
            DeviceResource horizontal;
            uint64_t rendered = 0;
            Microsoft::WRL::ComPtr<ID3D12Fence> outputReleased;
            uint64_t outputReleasedValue = 0;
        };
        Resources m_resources[RESOURCE_COUNT];
        DXGI_FORMAT m_normalFormat = DXGI_FORMAT_R16G16_FLOAT;
        DXGI_FORMAT m_depthFormat = DXGI_FORMAT_R32_FLOAT;
        DeviceResource m_random;
        DeviceResource m_randomUpload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_randomFootprint = {};
        bool m_randomUploaded = false;
        bool m_frameworkStyle = false;
        bool m_mailboxStyle = false;
        bool m_reconstructNormals = false;
        bool m_packedFrameworkDepth = false;
        bool m_packedFrameworkNormals = false;
        DeviceResource m_packedDepth;
        DeviceResource m_packedNormals;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_unpackRtvHeap;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_unpackPso;
        Microsoft::WRL::ComPtr<ID3D12Heap> m_sharedHeaps[3];
        Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTextures[3];
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        eastl::vector<eastl::unique_ptr<Recording>> m_recordings;
        ID3D12GraphicsCommandList* m_list = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_dependencies[2 + RESOURCE_COUNT];
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_rawPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_filterPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_horizontalPso;
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

    HRESULT DeviceSsaoPass::execute(const d3d::mgpu::SsaoConstants& constants, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady, uint64_t frame, uint32_t resourceIndex)
    {
        collectRetiredResources();
        return m_state && !m_state->m_frameworkStyle && resourceIndex < RESOURCE_COUNT ? m_state->execute(constants, normalsReady, depthReady, frame, resourceIndex) : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT DeviceSsaoPass::executeFramework(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared, uint64_t frame)
    {
        collectRetiredResources();
        if (!m_state || !m_state->m_frameworkStyle || !shared.depth || !shared.result ||
            (!m_state->m_reconstructNormals && !shared.normals)) return E_INVALIDARG;
        ID3D12Resource* textures[] = {shared.normals, shared.depth, shared.result};
        ID3D12Resource* locals[] = {m_state->m_packedFrameworkNormals ? m_state->m_packedNormals.getResource() : getNormals(),
            m_state->m_packedFrameworkDepth ? m_state->m_packedDepth.getResource() : getDepth(), getResult()};
        for (UINT index = 0; index < 3; ++index)
        {
            if (index == 0 && m_state->m_reconstructNormals) continue;
            // Resource descriptions are immutable. Validate and retain each borrowed texture only once.
            if (m_state->m_sharedTextures[index].Get() == textures[index]) continue;
            if (m_state->m_sharedTextures[index]) return E_INVALIDARG;
            const auto a = textures[index]->GetDesc();
            const auto b = locals[index]->GetDesc();
            Microsoft::WRL::ComPtr<ID3D12Device> owner;
            if (FAILED(textures[index]->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != m_state->m_device.Get() ||
                a.Width != b.Width || a.Height != b.Height || a.Format != b.Format ||
                (m_state->m_sharedTextures[index] && m_state->m_sharedTextures[index].Get() != textures[index])) return E_INVALIDARG;
            m_state->m_sharedTextures[index] = textures[index];
            m_state->m_sharedHeaps[index] = shared.heaps[index];
        }
        return m_state->execute(constants, {}, {}, frame, 0, &shared);
    }

    d3d::mgpu::SsaoGpuTiming DeviceSsaoPass::pollTiming()
    {
        if (!m_state) return {};
        m_state->pollTiming();
        return m_state->m_timing;
    }

    HRESULT DeviceSsaoPass::executeMailbox(const d3d::mgpu::SsaoConstants& constants, const FrameworkSsaoTextures& shared,
        const MailboxSsaoJob& job, uint64_t frame, MailboxSsaoCompletion& completion)
    {
        completion = {};
        if (!m_state || !m_state->m_mailboxStyle || !shared.depth || !shared.result ||
            (!m_state->m_reconstructNormals && !shared.normals) || !job.completion || !job.frameId ||
            !job.inputsReady.fence || !job.inputsReady.value || job.outputSlot >= MAILBOX_OUT_SLOTS ||
            (job.publishMailbox && !job.mailbox) || !frame || frame > UINT32_MAX) return E_INVALIDARG;
        // Slot descriptions match the resources allocated with this pass. Slots are owned and retained by the producer.
        return m_state->execute(constants, {}, {}, frame, 0, &shared, &job, &completion);
    }

    void DeviceSsaoPass::retainMailboxResources(const eastl::shared_ptr<void>& resources)
    {
        if (m_state && m_state->m_mailboxStyle) m_state->m_mailboxOwner = resources;
    }

    HRESULT DeviceSsaoPass::releaseOutput(CrossAdapterSyncPoint released, uint32_t resourceIndex)
    {
        return m_state && !m_state->m_frameworkStyle && resourceIndex < RESOURCE_COUNT ? m_state->releaseOutput(released, resourceIndex) : DXGI_ERROR_INVALID_CALL;
    }

    CrossAdapterSyncPoint DeviceSsaoPass::getReadyPoint(uint32_t resourceIndex) const
    {
        return m_state && resourceIndex < RESOURCE_COUNT && m_state->m_resources[resourceIndex].rendered
                   ? CrossAdapterSyncPoint{m_state->m_fence.Get(), m_state->m_resources[resourceIndex].rendered}
                   : CrossAdapterSyncPoint{};
    }

    ID3D12Resource* DeviceSsaoPass::getNormals(uint32_t resourceIndex) const
    {
        return m_state && resourceIndex < RESOURCE_COUNT ? m_state->m_resources[resourceIndex].normals.getResource() : nullptr;
    }
    ID3D12Resource* DeviceSsaoPass::getDepth(uint32_t resourceIndex) const
    {
        return m_state && resourceIndex < RESOURCE_COUNT ? m_state->m_resources[resourceIndex].depth.getResource() : nullptr;
    }
    ID3D12Resource* DeviceSsaoPass::getResult(uint32_t resourceIndex) const
    {
        return m_state && resourceIndex < RESOURCE_COUNT ? m_state->m_resources[resourceIndex].output.getResource() : nullptr;
    }

    ID3D12CommandQueue* DeviceSsaoPass::getQueue() const
    {
        return m_state ? m_state->m_queue.Get() : nullptr;
    }

    HRESULT DeviceSsaoPass::State::init(const DeviceSsaoPassDesc& desc)
    {
        m_device = desc.allocator->getDevice();
        m_allocator = desc.allocator;
        m_mailboxStyle = desc.mailboxStyle;
        m_frameworkStyle = desc.frameworkStyle || desc.mailboxStyle;
        m_reconstructNormals = desc.reconstructNormals;
        m_packedFrameworkDepth = m_frameworkStyle && desc.packedFrameworkDepth;
        m_packedFrameworkNormals = m_packedFrameworkDepth && desc.packedFrameworkNormals && !m_reconstructNormals;
        m_timing.packedDepth = m_packedFrameworkDepth;
        m_timing.packedNormals = m_packedFrameworkNormals;
        m_width = desc.width;
        m_height = desc.height;
        m_normalFormat = desc.packedNormals ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_FLOAT;
        m_depthFormat = desc.linearDepth16 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT result = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
        if (SUCCEEDED(result) && desc.measureTimings)
            result = m_queue->GetTimestampFrequency(&m_timestampFrequency);
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
        for (UINT index = 0; index < (m_frameworkStyle ? 1u : RESOURCE_COUNT); ++index)
        {
            auto& resources = m_resources[index];
            if (SUCCEEDED(result))
                result = resources.normals.createTexture(m_allocator.get(), m_frameworkStyle && m_reconstructNormals ? 1u : (desc.inputWidth ? desc.inputWidth : m_width),
                                              m_frameworkStyle && m_reconstructNormals ? 1u : (desc.inputHeight ? desc.inputHeight : m_height), m_normalFormat,
                                              m_packedFrameworkNormals ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
            if (SUCCEEDED(result))
                result = resources.depth.createTexture(m_allocator.get(), desc.inputWidth ? desc.inputWidth : m_width,
                                            desc.inputHeight ? desc.inputHeight : m_height, m_depthFormat,
                                            m_packedFrameworkDepth ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
            if (SUCCEEDED(result))
                result = resources.output.createTexture(m_allocator.get(), m_width, m_height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            if (SUCCEEDED(result))
                result = resources.raw.createTexture(m_allocator.get(), m_width, m_height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            if (SUCCEEDED(result))
                result = resources.horizontal.createTexture(m_allocator.get(), m_width, m_height, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
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
        if (SUCCEEDED(result)) result = createRandomTexture();
        if (SUCCEEDED(result))
        {
            result = createDescriptors();
        }
        if (SUCCEEDED(result))
        {
            result = createPipelines(desc);
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
        const UINT resourceCount = m_frameworkStyle ? 1u : RESOURCE_COUNT;
        desc.NumDescriptors = 3 * resourceCount;
        HRESULT result = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = SRVS_PER_SET * resourceCount;
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
            if (FAILED(result)) return result;
            m_device->CreateRenderTargetView(m_resources[0].depth.getResource(), nullptr, m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart());
            if (m_packedFrameworkNormals)
            {
                auto handle = m_unpackRtvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += m_rtvStride;
                m_device->CreateRenderTargetView(m_resources[0].normals.getResource(), nullptr, handle);
            }
        }
        for (UINT resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
        {
            auto& resources = m_resources[resourceIndex];
            ID3D12Resource* targets[] = {resources.output.getResource(), resources.raw.getResource(), resources.horizontal.getResource()};
            for (UINT i = 0; i < 3; ++i)
            {
                auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (resourceIndex * 3 + i) * m_rtvStride;
                m_device->CreateRenderTargetView(targets[i], nullptr, handle);
            }
            for (UINT table = 0; table < SRV_TABLES; ++table)
            {
                ID3D12Resource* inputs[] = {table == 3 && m_packedFrameworkNormals ? m_packedNormals.getResource() : resources.normals.getResource(), resources.depth.getResource(),
                    table == 2 ? resources.horizontal.getResource() : resources.raw.getResource(),
                    table == 3 && m_packedFrameworkDepth ? m_packedDepth.getResource() : resources.depth.getResource(), m_random.getResource()};
                for (UINT input = 0; input < 5; ++input)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                    srv.Format = inputs[input]->GetDesc().Format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += (resourceIndex * SRVS_PER_SET + table * SRVS_PER_TABLE + input) * m_srvStride;
                    m_device->CreateShaderResourceView(inputs[input], &srv, handle);
                }
            }
        }
        return S_OK;
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
        if (SUCCEEDED(result) && validShader(desc.filterShader))
        {
            pso.PS = desc.filterShader;
            result = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_filterPso));
            pso.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
            if (SUCCEEDED(result)) result = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_horizontalPso));
        }
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
        return result;
    }

    HRESULT DeviceSsaoPass::State::signal()
    {
        const HRESULT result = m_queue->Signal(m_fence.Get(), m_serial);
        m_signalPending = FAILED(result);
        return result;
    }

    HRESULT DeviceSsaoPass::State::releaseOutput(CrossAdapterSyncPoint released, uint32_t resourceIndex)
    {
        if (!released.fence) return released.value == 0 ? S_OK : E_INVALIDARG;
        if (released.value == UINT64_MAX || m_signalPending) return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (FAILED(released.fence->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != m_device.Get()) return E_INVALIDARG;
        auto& resources = m_resources[resourceIndex];
        if (m_dependencies[2 + resourceIndex] && m_dependencies[2 + resourceIndex].Get() != released.fence)
            return E_INVALIDARG;
        // Replacing an unconsumed release would lose the dependency on an earlier output reader.
        if (resources.outputReleased && (resources.outputReleased.Get() != released.fence || released.value < resources.outputReleasedValue))
            return E_INVALIDARG;
        resources.outputReleased = released.fence;
        resources.outputReleasedValue = released.value;
        return S_OK;
    }

    HRESULT DeviceSsaoPass::State::waitOn(CrossAdapterSyncPoint point, UINT index)
    {
        if (!point.fence)
        {
            return point.value == 0 ? S_OK : E_INVALIDARG;
        }
        if (m_signalPending || point.value == UINT64_MAX || m_serial >= UINT64_MAX - 1)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (FAILED(point.fence->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != m_device.Get() ||
            (m_dependencies[index] && m_dependencies[index].Get() != point.fence))
        {
            return E_INVALIDARG;
        }
        m_dependencies[index] = point.fence;
        const HRESULT result = m_queue->Wait(point.fence, point.value);
        if (FAILED(result))
        {
            return result;
        }
        ++m_serial;
        return signal();
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
        // The framework experiment uses exactly three frame recordings, reusing only completed ones.
        // The synchronized transport keeps its existing growing command-storage pool.
        for (const eastl::unique_ptr<Recording>& candidate : m_recordings)
        {
            if (candidate->fenceValue <= completed)
            {
                recording = candidate.get();
                HRESULT result = recording->allocator->Reset();
                return SUCCEEDED(result) ? recording->list->Reset(recording->allocator.Get(), m_rawPso.Get()) : result;
            }
        }
        if (m_frameworkStyle && m_recordings.size() >= (m_mailboxStyle ? MAILBOX_IN_SLOTS : 3u))
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
            if (m_mailboxStyle)
            {
                result = next->list->Close();
                if (SUCCEEDED(result)) result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next->allocator.Get(), nullptr, IID_PPV_ARGS(&next->publication));
                if (SUCCEEDED(result)) result = next->publication->Close();
                if (SUCCEEDED(result)) result = next->list->Reset(next->allocator.Get(), m_rawPso.Get());
            }
        }
        if (SUCCEEDED(result))
        {
            result = next->constants.createBuffer(m_allocator.get(), MAILBOX_CONTROL_OFFSET + (m_mailboxStyle ? 512u : 0u), D3D12_HEAP_TYPE_UPLOAD);
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
                if (SUCCEEDED(result)) result = next->timestampReadback.createBuffer(m_allocator.get(), TIMESTAMP_COUNT * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
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
        if (!m_fence) return;
        const uint64_t completed = m_fence->GetCompletedValue();
        if (completed == UINT64_MAX) return;
        for (auto& recording : m_recordings)
        {
            if (!recording->timingPending || recording->fenceValue > completed) continue;
            ++m_timing.completedJobs;
            const auto* t = recording->timestamps;
            if (m_timestampFrequency && t && recording->timingFrame > m_timing.frame &&
                t[RAW_BEGIN] >= t[INPUT_BEGIN] && t[RAW_END] >= t[RAW_BEGIN] &&
                t[FILTER_END] >= t[RAW_END] && t[OUTPUT_END] >= t[FILTER_END])
            {
                const auto ms = [&](Timestamp first, Timestamp last) {
                    return static_cast<float>(1000.0 * static_cast<double>(t[last] - t[first]) / m_timestampFrequency);
                };
                m_timing.frame = recording->timingFrame;
                m_timing.rawMilliseconds = ms(RAW_BEGIN, RAW_END);
                m_timing.totalMilliseconds = ms(RAW_BEGIN, FILTER_END);
                m_timing.blurMilliseconds = ms(RAW_END, FILTER_END);
                if (m_frameworkStyle)
                {
                    m_timing.inputMilliseconds = ms(INPUT_BEGIN, RAW_BEGIN);
                    m_timing.outputMilliseconds = ms(FILTER_END, OUTPUT_END);
                    m_timing.jobMilliseconds = ms(INPUT_BEGIN, OUTPUT_END);
                }
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
        // reset must include copies still reading either output, even if that set was never reused.
        for (uint32_t index = 0; index < RESOURCE_COUNT; ++index)
        {
            auto& resources = m_resources[index];
            if (!resources.outputReleased) continue;
            const HRESULT result = waitOn({resources.outputReleased.Get(), resources.outputReleasedValue}, 2 + index);
            if (FAILED(result)) return result;
            resources.outputReleased.Reset();
            resources.outputReleasedValue = 0;
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

    HRESULT DeviceSsaoPass::State::execute(const d3d::mgpu::SsaoConstants& data, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady, uint64_t frame, uint32_t resourceIndex, const FrameworkSsaoTextures* shared,
        const MailboxSsaoJob* mailbox, MailboxSsaoCompletion* completion)
    {
        auto& resources = m_resources[resourceIndex];
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
        if (recording->timestampHeap) m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, INPUT_BEGIN);
        if (shared)
        {
            // Contents are selected when these GPU commands execute, without a cross-adapter wait.
            // Reconstructed normals only sample depth; copying normal pixels wastes cross-adapter bandwidth.
            if (!m_reconstructNormals) copyShared(m_packedFrameworkNormals ? m_packedNormals.getResource() : resources.normals.getResource(), shared->normals, false);
            copyShared(m_packedFrameworkDepth ? m_packedDepth.getResource() : resources.depth.getResource(), shared->depth, false);
        }
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
            barrier(m_random.getResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
            table.ptr += (resourceIndex * SRVS_PER_SET + 3 * SRVS_PER_TABLE) * m_srvStride;
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
        barrier(resources.depth.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(resources.normals.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool filtered = data.filter[3] > 1.f && data.pipeline[0] >= 1.f && data.pipeline[0] <= 3.f && m_filterPso;
        const bool gaussian = data.pipeline[0] == 3.f;
        const auto draw = [&](ID3D12Resource* target, UINT rtvIndex, UINT table, UINT constantIndex,
                              ID3D12PipelineState* pso, const d3d::mgpu::SsaoConstants& constants)
        {
            std::memcpy(recording->mapped + constantIndex * CONSTANT_BYTES, &constants, sizeof(constants));
            m_list->SetGraphicsRootConstantBufferView(0, recording->constants.getResource()->GetGPUVirtualAddress() + constantIndex * CONSTANT_BYTES);
            auto srv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += (resourceIndex * SRVS_PER_SET + table * SRVS_PER_TABLE) * m_srvStride;
            m_list->SetGraphicsRootDescriptorTable(1, srv);
            m_list->SetPipelineState(pso);
            barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (resourceIndex * 3 + rtvIndex) * m_rtvStride;
            m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            m_list->DrawInstanced(3, 1, 0, 0);
            barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        };
        if (recording->timestampHeap) m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RAW_BEGIN);
        draw(filtered ? resources.raw.getResource() : resources.output.getResource(), filtered ? 1 : 0, 0, 0, m_rawPso.Get(), data);
        if (recording->timestampHeap) m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, RAW_END);
        if (filtered)
        {
            barrier(resources.raw.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto constants = data;
            constants.pipeline[1] = 0.f;
            draw(gaussian ? resources.horizontal.getResource() : resources.output.getResource(), gaussian ? 2 : 0, 1, 1,
                 gaussian ? m_horizontalPso.Get() : m_filterPso.Get(), constants);
            if (gaussian)
            {
                barrier(resources.horizontal.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                constants.pipeline[1] = 1.f;
                draw(resources.output.getResource(), 0, 2, 2, m_filterPso.Get(), constants);
                barrier(resources.horizontal.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            barrier(resources.raw.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        barrier(resources.normals.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(resources.depth.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FILTER_END);
            recording->timingFrame = frame;
        }
        if (shared) copyShared(resources.output.getResource(), shared->result, true);
        if (mailbox)
        {
            const uint32_t id = static_cast<uint32_t>(frame);
            std::memcpy(recording->mapped + MAILBOX_CONTROL_OFFSET, &id, sizeof(id));
            D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
            source.pResource = recording->constants.getResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset = MAILBOX_CONTROL_OFFSET;
            source.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
            destination.pResource = mailbox->frameId;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            barrier(mailbox->frameId, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            barrier(mailbox->frameId, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        if (recording->timestampHeap)
        {
            m_list->EndQuery(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, OUTPUT_END);
            m_list->ResolveQueryData(recording->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, TIMESTAMP_COUNT, recording->timestampReadback.getResource(), 0);
        }

        result = m_list->Close();
        if (SUCCEEDED(result))
        {
            result = waitOn(normalsReady, 0);
        }
        if (SUCCEEDED(result))
        {
            result = waitOn(depthReady, 1);
        }
        if (SUCCEEDED(result) && resources.outputReleased)
        {
            result = waitOn({resources.outputReleased.Get(), resources.outputReleasedValue}, 2 + resourceIndex);
            if (SUCCEEDED(result))
            {
                resources.outputReleased.Reset();
                resources.outputReleasedValue = 0;
            }
        }
        if (FAILED(result))
        {
            return result;
        }
        if (mailbox && mailbox->publishMailbox)
        {
            // Pre-record B before submitting A; no fallible command recording remains between data and publication.
            result = recording->publication->Reset(recording->allocator.Get(), nullptr);
            if (FAILED(result)) return result;
            uint64_t entries[MAILBOX_OUT_SLOTS] = {};
            entries[mailbox->outputSlot] = frame;
            std::memcpy(recording->mapped + MAILBOX_CONTROL_OFFSET + 256, entries, sizeof(entries));
            D3D12_RESOURCE_BARRIER transition = {};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition = {mailbox->mailbox, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            recording->publication->ResourceBarrier(1, &transition);
            recording->publication->CopyBufferRegion(mailbox->mailbox, 0, recording->constants.getResource(), MAILBOX_CONTROL_OFFSET + 256, sizeof(entries));
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            recording->publication->ResourceBarrier(1, &transition);
            result = recording->publication->Close();
            if (FAILED(result)) return result;
        }
        if (mailbox)
        {
            // Only GPU1 waits. F0 covers this frame's inputs and old GPU0 readers of a reused
            // output slot. Do not route this through waitOn(), which adds a local signal.
            result = m_queue->Wait(mailbox->inputsReady.fence, mailbox->inputsReady.value);
            if (FAILED(result)) return result;
        }
        ID3D12CommandList* lists[] = {m_list};
        m_queue->ExecuteCommandLists(1, lists);
        m_randomUploaded = true;

        if (mailbox)
        {
            // Record ownership as soon as A is submitted, including every subsequent failing Signal path.
            recording->fenceValue = ++m_serial;
            resources.rendered = recording->fenceValue;
            recording->timingPending = true;
            ++m_timing.submittedJobs;
            completion->dataReady = mailbox->completion->enqueueSignal(m_queue.Get());
            if (!completion->dataReady) { signal(); return E_FAIL; }
            if (mailbox->publishMailbox)
            {
                ID3D12CommandList* publish[] = {recording->publication.Get()};
                m_queue->ExecuteCommandLists(1, publish);
                completion->published = mailbox->completion->enqueueSignal(m_queue.Get());
                if (!completion->published) { signal(); return E_FAIL; }
            }
            else completion->published = completion->dataReady;
        }

        if (!mailbox)
        {
            recording->fenceValue = ++m_serial;
            resources.rendered = recording->fenceValue;
            recording->timingPending = true;
            ++m_timing.submittedJobs;
        }
        return signal();
    }
}  // namespace drv3d_dx12
