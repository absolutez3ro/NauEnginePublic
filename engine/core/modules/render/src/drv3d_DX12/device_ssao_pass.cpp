// device_ssao_pass.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "device_ssao_pass.h"

#include <cstring>

namespace drv3d_dx12
{
    namespace
    {
        constexpr UINT FRAME_COUNT = 2;
        constexpr UINT PASS_COUNT = 3;
        constexpr UINT CONSTANT_BYTES = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

        bool validShader(const D3D12_SHADER_BYTECODE& shader)
        {
            return shader.pShaderBytecode && shader.BytecodeLength != 0;
        }
    }  // namespace

    class DeviceSsaoPass::State
    {
    public:
        ~State()
        {
            if (profiler)
            {
                profiler->detach();
            }
            if (mapped)
            {
                constants.getResource()->Unmap(0, nullptr);
            }
            if (event)
            {
                CloseHandle(event);
            }
        }

        HRESULT init(const DeviceSsaoPassDesc& desc);
        HRESULT createDescriptors();
        HRESULT createPipelines(const DeviceSsaoPassDesc& desc);
        HRESULT canSubmit() const;
        HRESULT execute(const d3d::mgpu::SsaoConstants& data, bool blur, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady);
        HRESULT wait(DWORD timeoutMs);
        HRESULT waitOn(CrossAdapterSyncPoint point, UINT index);
        HRESULT signal();
        void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
        bool removed() const
        {
            return device && FAILED(device->GetDeviceRemovedReason());
        }

        // Resources are destroyed before the shared allocator and device, including after retirement.
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        eastl::shared_ptr<IDeviceResourceAllocator> allocator;
        eastl::shared_ptr<d3d::mgpu::ISsaoProfiler> profiler;
        DeviceResource normals;
        DeviceResource depth;
        DeviceResource targets[PASS_COUNT];
        DeviceResource constants;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[FRAME_COUNT];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Fence> dependencies[4];
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> rawPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> blurPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> outputPso;
        HANDLE event = nullptr;
        uint8_t* mapped = nullptr;
        UINT width = 0;
        UINT height = 0;
        UINT srvStride = 0;
        UINT rtvStride = 0;
        UINT64 serial = 0;
        UINT64 submissions = 0;
        UINT64 slotValues[FRAME_COUNT] = {};
        bool signalPending = false;
        State* nextRetired = nullptr;
    };

    DeviceSsaoPass::State* DeviceSsaoPass::s_retired = nullptr;
    DeviceSsaoPass::DeviceSsaoPass() = default;

    DeviceSsaoPass::~DeviceSsaoPass()
    {
        if (m_state && m_state->profiler)
        {
            m_state->profiler->detach();
        }
        reset(0);
        if (m_state)
        {
            m_state->nextRetired = s_retired;
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
            !validShader(desc.vertexShader) || !validShader(desc.rawShader) || !validShader(desc.blurShader))
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
                *current = state->nextRetired;
                delete state;
            }
            else
            {
                ++pending;
                current = &state->nextRetired;
            }
        }
        return pending;
    }

    HRESULT DeviceSsaoPass::canSubmit() const
    {
        return m_state ? m_state->canSubmit() : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT DeviceSsaoPass::execute(const d3d::mgpu::SsaoConstants& constants, bool blur, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady)
    {
        collectRetiredResources();
        return m_state ? m_state->execute(constants, blur, normalsReady, depthReady) : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT DeviceSsaoPass::releaseOutput(CrossAdapterSyncPoint released, bool raw)
    {
        return m_state ? m_state->waitOn(released, raw ? 3 : 2) : DXGI_ERROR_INVALID_CALL;
    }

    CrossAdapterSyncPoint DeviceSsaoPass::getReadyPoint() const
    {
        return m_state ? CrossAdapterSyncPoint{m_state->fence.Get(), m_state->serial} : CrossAdapterSyncPoint{};
    }

    ID3D12Resource* DeviceSsaoPass::getNormals() const
    {
        return m_state ? m_state->normals.getResource() : nullptr;
    }
    ID3D12Resource* DeviceSsaoPass::getDepth() const
    {
        return m_state ? m_state->depth.getResource() : nullptr;
    }
    ID3D12Resource* DeviceSsaoPass::getResult(bool raw) const
    {
        return m_state ? m_state->targets[raw ? 0 : 2].getResource() : nullptr;
    }

    HRESULT DeviceSsaoPass::State::init(const DeviceSsaoPassDesc& desc)
    {
        device = desc.allocator->getDevice();
        allocator = desc.allocator;
        width = desc.width;
        height = desc.height;
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT result = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
        for (UINT slot = 0; SUCCEEDED(result) && slot < FRAME_COUNT; ++slot)
        {
            result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[slot]));
        }
        if (SUCCEEDED(result))
        {
            result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&list));
        }
        if (SUCCEEDED(result))
        {
            result = list->Close();
        }
        if (SUCCEEDED(result))
        {
            result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        }
        if (SUCCEEDED(result))
        {
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event)
            {
                result = HRESULT_FROM_WIN32(GetLastError());
            }
        }
        if (SUCCEEDED(result))
        {
            result = normals.createTexture(allocator.get(), width, height, DXGI_FORMAT_R16G16_FLOAT);
        }
        // CrossAdapterChannel transports raw rows, so an R32_FLOAT endpoint receives the engine's typeless DEPTH32 bytes.
        if (SUCCEEDED(result))
        {
            result = depth.createTexture(allocator.get(), width, height, DXGI_FORMAT_R32_FLOAT);
        }
        for (UINT pass = 0; SUCCEEDED(result) && pass < PASS_COUNT; ++pass)
        {
            result = targets[pass].createTexture(allocator.get(), width, height, pass == 2 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        }
        if (SUCCEEDED(result))
        {
            result = constants.createBuffer(allocator.get(), FRAME_COUNT * PASS_COUNT * CONSTANT_BYTES, D3D12_HEAP_TYPE_UPLOAD);
        }
        if (SUCCEEDED(result))
        {
            const D3D12_RANGE noRead = {0, 0};
            void* data = nullptr;
            result = constants.getResource()->Map(0, &noRead, &data);
            mapped = static_cast<uint8_t*>(data);
        }
        if (SUCCEEDED(result))
        {
            result = createDescriptors();
        }
        if (SUCCEEDED(result))
        {
            result = createPipelines(desc);
        }
        if (SUCCEEDED(result) && desc.profiler)
        {
            profiler = desc.profiler;
            profiler->attach(device.Get(), queue.Get());
        }
        return result;
    }

    HRESULT DeviceSsaoPass::State::createDescriptors()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = PASS_COUNT;
        HRESULT result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvHeap));
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = PASS_COUNT * 3;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (SUCCEEDED(result))
        {
            result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvHeap));
        }
        if (FAILED(result))
        {
            return result;
        }
        srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT pass = 0; pass < PASS_COUNT; ++pass)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += pass * rtvStride;
            device->CreateRenderTargetView(targets[pass].getResource(), nullptr, rtv);
            ID3D12Resource* inputs[] = {normals.getResource(), depth.getResource(), pass == 0 ? depth.getResource() : targets[pass - 1].getResource()};
            for (UINT input = 0; input < 3; ++input)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = input == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (pass * 3 + input) * srvStride;
                device->CreateShaderResourceView(inputs[input], &srv, handle);
            }
        }
        return S_OK;
    }

    HRESULT DeviceSsaoPass::State::createPipelines(const DeviceSsaoPassDesc& desc)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
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
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors);
        if (SUCCEEDED(result))
        {
            result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
        }
        if (FAILED(result))
        {
            return result;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = root.Get();
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
        pso.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        pso.SampleDesc.Count = 1;
        result = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&rawPso));
        pso.PS = desc.blurShader;
        if (SUCCEEDED(result))
        {
            result = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&blurPso));
        }
        pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(result))
        {
            result = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outputPso));
        }
        return result;
    }

    HRESULT DeviceSsaoPass::State::signal()
    {
        const HRESULT result = queue->Signal(fence.Get(), serial);
        signalPending = FAILED(result);
        return result;
    }

    HRESULT DeviceSsaoPass::State::waitOn(CrossAdapterSyncPoint point, UINT index)
    {
        if (!point.fence)
        {
            return point.value == 0 ? S_OK : E_INVALIDARG;
        }
        if (signalPending || point.value == UINT64_MAX || serial >= UINT64_MAX - 1)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (FAILED(point.fence->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device.Get() ||
            (dependencies[index] && dependencies[index].Get() != point.fence))
        {
            return E_INVALIDARG;
        }
        dependencies[index] = point.fence;
        const HRESULT result = queue->Wait(point.fence, point.value);
        if (FAILED(result))
        {
            return result;
        }
        ++serial;
        return signal();
    }

    HRESULT DeviceSsaoPass::State::canSubmit() const
    {
        if (signalPending || serial >= UINT64_MAX - 4)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        return completed >= slotValues[submissions % FRAME_COUNT] ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
    }

    HRESULT DeviceSsaoPass::State::wait(DWORD timeoutMs)
    {
        if (!fence)
        {
            return S_OK;
        }
        if (signalPending)
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
            const UINT64 completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            if (completed >= serial)
            {
                return S_OK;
            }
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            const HRESULT result = fence->SetEventOnCompletion(serial, event);
            if (FAILED(result))
            {
                return result;
            }
            const DWORD status = WaitForSingleObject(event, timeoutMs - static_cast<DWORD>(elapsed));
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
        list->ResourceBarrier(1, &transition);
    }

    HRESULT DeviceSsaoPass::State::execute(const d3d::mgpu::SsaoConstants& data, bool blur, CrossAdapterSyncPoint normalsReady, CrossAdapterSyncPoint depthReady)
    {
        HRESULT result = canSubmit();
        if (result != S_OK)
        {
            return result;
        }
        const UINT slot = static_cast<UINT>(submissions % FRAME_COUNT);
        result = commandAllocators[slot]->Reset();
        if (SUCCEEDED(result))
        {
            result = list->Reset(commandAllocators[slot].Get(), rawPso.Get());
        }
        if (FAILED(result))
        {
            return result;
        }
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root.Get());
        const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        barrier(normals.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(depth.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for (UINT pass = 0; pass < PASS_COUNT; ++pass)
        {
            if (profiler)
            {
                constexpr const char* names[] = {"ssao_raw", "ssao_blur_h", "ssao_blur_v"};
                profiler->beginPass(list.Get(), names[pass]);
            }
            d3d::mgpu::SsaoConstants parameters = data;
            parameters.filter[0] = blur && pass == 1 ? 1.0f : 0.0f;
            parameters.filter[1] = blur && pass == 2 ? 1.0f : 0.0f;
            const UINT offset = (slot * PASS_COUNT + pass) * CONSTANT_BYTES;
            std::memcpy(mapped + offset, &parameters, sizeof(parameters));
            list->SetGraphicsRootConstantBufferView(0, constants.getResource()->GetGPUVirtualAddress() + offset);
            D3D12_GPU_DESCRIPTOR_HANDLE srv = srvHeap->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += pass * 3 * srvStride;
            list->SetGraphicsRootDescriptorTable(1, srv);
            list->SetPipelineState(pass == 0 ? rawPso.Get() : (pass == 1 ? blurPso.Get() : outputPso.Get()));
            ID3D12Resource* target = targets[pass].getResource();
            barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->DiscardResource(target, nullptr);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += pass * rtvStride;
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    pass == 2 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (profiler)
            {
                profiler->endPass();
            }
        }
        for (UINT pass = 0; pass < 2; ++pass)
        {
            barrier(targets[pass].getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        barrier(normals.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(depth.getResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (profiler)
        {
            profiler->resolve(list.Get());
        }
        result = list->Close();
        if (SUCCEEDED(result))
        {
            result = waitOn(normalsReady, 0);
        }
        if (SUCCEEDED(result))
        {
            result = waitOn(depthReady, 1);
        }
        if (FAILED(result))
        {
            return result;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (profiler)
        {
            profiler->submitted();
        }
        slotValues[slot] = ++serial;
        ++submissions;
        return signal();
    }
}  // namespace drv3d_dx12
