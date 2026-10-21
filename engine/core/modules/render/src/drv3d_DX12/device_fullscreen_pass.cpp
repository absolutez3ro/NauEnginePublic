// Copyright 2026 N-GINN LLC. All rights reserved.

#include "device_fullscreen_pass.h"

#include <cstring>

namespace drv3d_dx12
{
    namespace
    {
        constexpr UINT64 CONSTANT_SLOT_SIZE = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        constexpr DXGI_FORMAT RENDER_TARGET_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

        bool isValidBytecode(const D3D12_SHADER_BYTECODE& bytecode)
        {
            return bytecode.pShaderBytecode != nullptr && bytecode.BytecodeLength != 0;
        }
    }  // namespace

    class DeviceFullscreenPass::State
    {
    public:
        State() = default;
        State(const State&) = delete;
        State(State&&) = delete;
        State& operator=(const State&) = delete;
        State& operator=(State&&) = delete;
        ~State() = default;  // Deleted only after reset() has released GPU state.

        HRESULT init(const DeviceFullscreenPassDesc& desc);
        HRESULT reset(DWORD timeoutMs = WAIT_TIMEOUT_MS);
        void setColor(const FullscreenPassColor& color);
        HRESULT execute(bool copyToReadback);
        HRESULT waitForCompletion(DWORD timeoutMs = WAIT_TIMEOUT_MS);
        HRESULT waitForExternalWork(ID3D12Fence* fence, UINT64 value);
        HRESULT readPixels(uint8_t* destination, size_t destinationSize) const;
        ID3D12Fence* getFence() const;
        UINT64 getLastSubmittedValue() const;
        ID3D12Resource* getRenderTarget() const;
        ID3D12PipelineState* getPipelineState() const;
        ID3D12RootSignature* getRootSignature() const;
        ID3D12Resource* getConstantBuffer() const;
        ID3D12Device* getDevice() const;

        State* m_nextRetired = nullptr;

    private:
        struct ExternalWait
        {
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            UINT64 completionValue = 0;
            ExternalWait* next = nullptr;
        };

        void collectExternalWaits(UINT64 completedValue);
        ExternalWait* m_externalWaits = nullptr;

    private:
        HRESULT createCommandObjects();
        HRESULT createPipeline(const DeviceFullscreenPassDesc& desc);
        HRESULT createResources(UINT width, UINT height);
        HRESULT waitForValue(UINT64 value, DWORD timeoutMs = WAIT_TIMEOUT_MS);
        HRESULT signalSubmission();
        void recordPass(UINT slot, bool copyToReadback);
        void recordBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    private:
        // Declared device first: members are destroyed in reverse order, so the device reference is released last.
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        eastl::shared_ptr<IDeviceResourceAllocator> m_resourceAllocator;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[FRAME_COUNT];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
        DeviceResource m_renderTarget;
        DeviceResource m_constantBuffer;  ///< UPLOAD heap, persistently mapped, one 256-byte slot per submission slot.
        DeviceResource m_readback;        ///< READBACK heap sized from GetCopyableFootprints of the render target.
        HANDLE m_fenceEvent = nullptr;
        uint8_t* m_constantsMapped = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint = {};
        UINT64 m_readbackBytes = 0;  ///< Total size from GetCopyableFootprints; the last row has no padding.
        UINT64 m_slotFenceValues[FRAME_COUNT] = {};
        UINT64 m_lastSubmittedValue = 0;
        UINT64 m_readbackValue = 0;  ///< Fence value of the most recent submission that copied into m_readback.
        bool m_signalPending = false;
        FullscreenPassColor m_color;
        UINT m_width = 0;
        UINT m_height = 0;
    };

    DeviceFullscreenPass::State* DeviceFullscreenPass::s_retiredStates = nullptr;

    DeviceFullscreenPass::DeviceFullscreenPass() = default;

    DeviceFullscreenPass::~DeviceFullscreenPass()
    {
        reset(0);
        if (m_state)
        {
            // The state was allocated by init(): retirement cannot fail to allocate in a destructor.
            m_state->m_nextRetired = s_retiredStates;
            s_retiredStates = m_state.release();
            OutputDebugStringA("MG-10: retiring unfinished GPU work; collectRetiredResources() must poll it.\n");
        }
    }

    size_t DeviceFullscreenPass::collectRetiredResources()
    {
        size_t pending = 0;
        State** current = &s_retiredStates;
        while (*current)
        {
            State* state = *current;
            state->reset(0);
            if (!state->getDevice())
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

    HRESULT DeviceFullscreenPass::init(const DeviceFullscreenPassDesc& desc)
    {
        collectRetiredResources();
        if (m_state)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        eastl::unique_ptr<State> state(new State());
        const HRESULT result = state->init(desc);
        if (SUCCEEDED(result))
        {
            state->setColor(m_color);
            m_state = eastl::move(state);
        }
        return result;
    }

    HRESULT DeviceFullscreenPass::reset(DWORD timeoutMs)
    {
        if (!m_state)
        {
            m_color = {};
            return S_OK;
        }
        const HRESULT result = m_state->reset(timeoutMs);
        if (!m_state->getDevice())
        {
            m_state.reset();
            m_color = {};
        }
        return result;
    }

    void DeviceFullscreenPass::setColor(const FullscreenPassColor& color)
    {
        m_color = color;
        if (m_state)
        {
            m_state->setColor(color);
        }
    }

    HRESULT DeviceFullscreenPass::execute(bool copyToReadback)
    {
        collectRetiredResources();
        return m_state ? m_state->execute(copyToReadback) : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT DeviceFullscreenPass::waitForCompletion(DWORD timeoutMs)
    {
        return m_state ? m_state->waitForCompletion(timeoutMs) : S_OK;
    }

    HRESULT DeviceFullscreenPass::waitForExternalWork(ID3D12Fence* fence, UINT64 value)
    {
        return m_state ? m_state->waitForExternalWork(fence, value) : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT DeviceFullscreenPass::readPixels(uint8_t* destination, size_t destinationSize) const
    {
        if (!destination)
        {
            return E_INVALIDARG;
        }
        return m_state ? m_state->readPixels(destination, destinationSize) : DXGI_ERROR_INVALID_CALL;
    }

    ID3D12Fence* DeviceFullscreenPass::getFence() const
    {
        return m_state ? m_state->getFence() : nullptr;
    }

    UINT64 DeviceFullscreenPass::getLastSubmittedValue() const
    {
        return m_state ? m_state->getLastSubmittedValue() : 0;
    }

    ID3D12Resource* DeviceFullscreenPass::getRenderTarget() const
    {
        return m_state ? m_state->getRenderTarget() : nullptr;
    }

    ID3D12PipelineState* DeviceFullscreenPass::getPipelineState() const
    {
        return m_state ? m_state->getPipelineState() : nullptr;
    }

    ID3D12RootSignature* DeviceFullscreenPass::getRootSignature() const
    {
        return m_state ? m_state->getRootSignature() : nullptr;
    }

    ID3D12Resource* DeviceFullscreenPass::getConstantBuffer() const
    {
        return m_state ? m_state->getConstantBuffer() : nullptr;
    }

    ID3D12Device* DeviceFullscreenPass::getDevice() const
    {
        return m_state ? m_state->getDevice() : nullptr;
    }

    HRESULT DeviceFullscreenPass::State::init(const DeviceFullscreenPassDesc& desc)
    {
        if (!desc.allocator || !desc.allocator->getDevice() || desc.width == 0 || desc.height == 0 || desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !isValidBytecode(desc.vertexShader) || !isValidBytecode(desc.pixelShader))
        {
            return E_INVALIDARG;
        }
        if (m_device)
        {
            // A working pass must never be replaced silently; the caller has to reset() it first.
            return DXGI_ERROR_INVALID_CALL;
        }

        m_resourceAllocator = desc.allocator;
        m_device = desc.allocator->getDevice();
        m_width = desc.width;
        m_height = desc.height;

        HRESULT result = createCommandObjects();
        if (SUCCEEDED(result))
        {
            result = createResources(desc.width, desc.height);
        }
        if (SUCCEEDED(result))
        {
            result = createPipeline(desc);
        }
        if (FAILED(result))
        {
            reset();
        }
        return result;
    }

    HRESULT DeviceFullscreenPass::State::createCommandObjects()
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT result = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
        for (UINT slot = 0; SUCCEEDED(result) && slot < FRAME_COUNT; ++slot)
        {
            result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[slot]));
        }
        if (SUCCEEDED(result))
        {
            result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_list));
        }
        if (SUCCEEDED(result))
        {
            // A new list starts in the recording state; execute() always begins with Reset().
            result = m_list->Close();
        }
        if (SUCCEEDED(result))
        {
            result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        }
        if (SUCCEEDED(result))
        {
            m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!m_fenceEvent)
            {
                result = HRESULT_FROM_WIN32(GetLastError());
            }
        }
        return result;
    }

    HRESULT DeviceFullscreenPass::State::createResources(UINT width, UINT height)
    {
        HRESULT result = m_renderTarget.createRenderTarget(m_resourceAllocator.get(), width, height);
        if (FAILED(result))
        {
            return result;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        result = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap));
        if (FAILED(result))
        {
            return result;
        }
        m_device->CreateRenderTargetView(m_renderTarget.getResource(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        result = m_constantBuffer.createBuffer(m_resourceAllocator.get(), CONSTANT_SLOT_SIZE * FRAME_COUNT, D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(result))
        {
            return result;
        }
        // Upload memory stays mapped for the lifetime of the pass; the CPU never reads it.
        const D3D12_RANGE noRead = {0, 0};
        void* mapped = nullptr;
        result = m_constantBuffer.getResource()->Map(0, &noRead, &mapped);
        if (FAILED(result))
        {
            return result;
        }
        m_constantsMapped = static_cast<uint8_t*>(mapped);
        std::memset(m_constantsMapped, 0, static_cast<size_t>(CONSTANT_SLOT_SIZE * FRAME_COUNT));

        const D3D12_RESOURCE_DESC targetDesc = m_renderTarget.getResource()->GetDesc();
        m_device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &m_footprint, nullptr, nullptr, &m_readbackBytes);
        return m_readback.createBuffer(m_resourceAllocator.get(), m_readbackBytes, D3D12_HEAP_TYPE_READBACK);
    }

    HRESULT DeviceFullscreenPass::State::createPipeline(const DeviceFullscreenPassDesc& desc)
    {
        // One root CBV (b0) visible to the pixel shader only.
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = 0;
        parameter.Descriptor.RegisterSpace = 0;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &parameter;
        // No input assembler layout is used: the vertex shader generates positions from SV_VertexID.
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> serializeError;
        HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &serializeError);
        if (FAILED(result))
        {
            return result;
        }
        result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = desc.vertexShader;
        psoDesc.PS = desc.pixelShader;
        D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
        blend.BlendEnable = FALSE;
        blend.LogicOpEnable = FALSE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ZERO;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        psoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        psoDesc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        psoDesc.DepthStencilState.BackFace = psoDesc.DepthStencilState.FrontFace;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = RENDER_TARGET_FORMAT;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        return m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    }

    HRESULT DeviceFullscreenPass::State::reset(DWORD timeoutMs)
    {
        const HRESULT result = waitForCompletion(timeoutMs);
        if (FAILED(result) && m_device && SUCCEEDED(m_device->GetDeviceRemovedReason()))
        {
            return result;
        }
        if (m_constantsMapped)
        {
            m_constantBuffer.getResource()->Unmap(0, nullptr);
            m_constantsMapped = nullptr;
        }
        m_pipelineState.Reset();
        m_rootSignature.Reset();
        m_rtvHeap.Reset();
        m_readback.reset();
        m_constantBuffer.reset();
        m_renderTarget.reset();
        m_list.Reset();
        for (Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocator : m_allocators)
        {
            allocator.Reset();
        }
        m_queue.Reset();
        collectExternalWaits(UINT64_MAX);
        m_fence.Reset();
        if (m_fenceEvent)
        {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        for (UINT64& value : m_slotFenceValues)
        {
            value = 0;
        }
        m_lastSubmittedValue = 0;
        m_readbackValue = 0;
        m_signalPending = false;
        m_footprint = {};
        m_readbackBytes = 0;
        m_color = FullscreenPassColor();
        m_width = 0;
        m_height = 0;
        m_resourceAllocator.reset();
        m_device.Reset();
        return result;
    }

    void DeviceFullscreenPass::State::setColor(const FullscreenPassColor& color)
    {
        m_color = color;
    }

    HRESULT DeviceFullscreenPass::State::waitForValue(UINT64 value, DWORD timeoutMs)
    {
        UINT64 completed = m_fence->GetCompletedValue();
        // A removed device completes every fence with UINT64_MAX.
        if (completed == UINT64_MAX)
        {
            const HRESULT reason = m_device->GetDeviceRemovedReason();
            return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
        }
        if (completed >= value)
        {
            return S_OK;
        }
        if (timeoutMs == 0)
        {
            // Polling must not register an unbounded number of events on a stuck fence.
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        HRESULT result = m_fence->SetEventOnCompletion(value, m_fenceEvent);
        if (FAILED(result))
        {
            return result;
        }
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            const ULONGLONG elapsed = GetTickCount64() - start;
            const DWORD remaining = timeoutMs == INFINITE ? INFINITE : (elapsed >= timeoutMs ? 0 : timeoutMs - static_cast<DWORD>(elapsed));
            const DWORD waitResult = WaitForSingleObject(m_fenceEvent, remaining);
            completed = m_fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                const HRESULT reason = m_device->GetDeviceRemovedReason();
                return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
            }
            if (completed >= value)
            {
                return S_OK;
            }
            if (waitResult == WAIT_TIMEOUT)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            if (waitResult != WAIT_OBJECT_0)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            // A registration from an earlier timed-out wait can wake this event before our value is reached.
        }
    }

    HRESULT DeviceFullscreenPass::State::waitForCompletion(DWORD timeoutMs)
    {
        if (!m_fence)
        {
            return S_OK;
        }
        if (m_signalPending)
        {
            const HRESULT result = signalSubmission();
            if (FAILED(result))
            {
                return result;
            }
        }
        return waitForValue(m_lastSubmittedValue, timeoutMs);
    }

    HRESULT DeviceFullscreenPass::State::signalSubmission()
    {
        const HRESULT result = m_queue->Signal(m_fence.Get(), m_lastSubmittedValue);
        m_signalPending = FAILED(result);
        return result;
    }

    HRESULT DeviceFullscreenPass::State::waitForExternalWork(ID3D12Fence* fence, UINT64 value)
    {
        if (!m_queue || m_signalPending || m_lastSubmittedValue == UINT64_MAX - 1)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        if (!fence || value == UINT64_MAX)
        {
            return E_INVALIDARG;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        const HRESULT ownerResult = fence->GetDevice(IID_PPV_ARGS(&owner));
        if (FAILED(ownerResult) || owner.Get() != m_device.Get())
        {
            return E_INVALIDARG;
        }
        collectExternalWaits(m_fence->GetCompletedValue());
        eastl::unique_ptr<ExternalWait> dependency(new ExternalWait());
        dependency->fence = fence;
        const HRESULT result = m_queue->Wait(fence, value);
        if (FAILED(result))
        {
            return result;
        }
        dependency->completionValue = ++m_lastSubmittedValue;
        dependency->next = m_externalWaits;
        m_externalWaits = dependency.release();
        return signalSubmission();
    }

    void DeviceFullscreenPass::State::collectExternalWaits(UINT64 completedValue)
    {
        ExternalWait** current = &m_externalWaits;
        while (*current)
        {
            ExternalWait* dependency = *current;
            if (completedValue >= dependency->completionValue)
            {
                *current = dependency->next;
                delete dependency;
            }
            else
            {
                current = &dependency->next;
            }
        }
    }

    void DeviceFullscreenPass::State::recordBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        m_list->ResourceBarrier(1, &barrier);
    }

    void DeviceFullscreenPass::State::recordPass(UINT slot, bool copyToReadback)
    {
        ID3D12Resource* target = m_renderTarget.getResource();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};

        recordBarrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Placed render targets need initialization before drawing, including reused heap ranges.
        // Every pixel is overwritten by the fullscreen triangle, so no clear color is needed.
        m_list->DiscardResource(target, nullptr);
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_list->RSSetViewports(1, &viewport);
        m_list->RSSetScissorRects(1, &scissor);
        m_list->SetGraphicsRootSignature(m_rootSignature.Get());
        m_list->SetGraphicsRootConstantBufferView(0, m_constantBuffer.getResource()->GetGPUVirtualAddress() + slot * CONSTANT_SLOT_SIZE);
        m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_list->DrawInstanced(3, 1, 0, 0);

        if (!copyToReadback)
        {
            recordBarrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            return;
        }
        recordBarrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = m_readback.getResource();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m_footprint;
        m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        recordBarrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    HRESULT DeviceFullscreenPass::State::execute(bool copyToReadback)
    {
        if (!m_list || m_signalPending || m_lastSubmittedValue == UINT64_MAX - 1)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        // Submission v (1-based) uses slot (v - 1) % FRAME_COUNT, so the next one uses lastSubmitted % FRAME_COUNT.
        // Only if the previous submission in this slot is still running does the CPU have to wait for it.
        const UINT slot = static_cast<UINT>(m_lastSubmittedValue % FRAME_COUNT);
        HRESULT result = waitForValue(m_slotFenceValues[slot]);
        if (FAILED(result))
        {
            return result;
        }
        result = m_allocators[slot]->Reset();
        if (FAILED(result))
        {
            return result;
        }
        result = m_list->Reset(m_allocators[slot].Get(), m_pipelineState.Get());
        if (FAILED(result))
        {
            return result;
        }

        std::memcpy(m_constantsMapped + slot * CONSTANT_SLOT_SIZE, &m_color, sizeof(m_color));
        recordPass(slot, copyToReadback);
        result = m_list->Close();
        if (FAILED(result))
        {
            return result;
        }

        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        const UINT64 value = m_lastSubmittedValue + 1;
        // Track work before Signal: even a failed Signal does not undo ExecuteCommandLists.
        m_lastSubmittedValue = value;
        m_slotFenceValues[slot] = value;
        if (copyToReadback)
        {
            m_readbackValue = value;
        }
        return signalSubmission();
    }

    HRESULT DeviceFullscreenPass::State::readPixels(uint8_t* destination, size_t destinationSize) const
    {
        const size_t rowBytes = static_cast<size_t>(m_width) * 4;
        if (!destination || destinationSize < rowBytes * m_height)
        {
            return E_INVALIDARG;
        }
        if (!m_fence || m_readbackValue == 0)
        {
            return DXGI_ERROR_INVALID_CALL;
        }
        const UINT64 completed = m_fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (completed < m_readbackValue)
        {
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }

        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(m_readbackBytes)};
        void* mapped = nullptr;
        const HRESULT result = m_readback.getResource()->Map(0, &readRange, &mapped);
        if (FAILED(result))
        {
            return result;
        }
        const uint8_t* source = static_cast<const uint8_t*>(mapped) + m_footprint.Offset;
        for (UINT y = 0; y < m_height; ++y)
        {
            std::memcpy(destination + y * rowBytes, source + static_cast<size_t>(y) * m_footprint.Footprint.RowPitch, rowBytes);
        }
        const D3D12_RANGE noWrite = {0, 0};
        m_readback.getResource()->Unmap(0, &noWrite);
        return S_OK;
    }

    ID3D12Fence* DeviceFullscreenPass::State::getFence() const
    {
        return m_fence.Get();
    }

    UINT64 DeviceFullscreenPass::State::getLastSubmittedValue() const
    {
        return m_lastSubmittedValue;
    }

    ID3D12Resource* DeviceFullscreenPass::State::getRenderTarget() const
    {
        return m_renderTarget.getResource();
    }

    ID3D12PipelineState* DeviceFullscreenPass::State::getPipelineState() const
    {
        return m_pipelineState.Get();
    }

    ID3D12RootSignature* DeviceFullscreenPass::State::getRootSignature() const
    {
        return m_rootSignature.Get();
    }

    ID3D12Resource* DeviceFullscreenPass::State::getConstantBuffer() const
    {
        return m_constantBuffer.getResource();
    }

    ID3D12Device* DeviceFullscreenPass::State::getDevice() const
    {
        return m_device.Get();
    }
}  // namespace drv3d_dx12
