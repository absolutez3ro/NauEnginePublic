// ao_xegtao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/ao_xegtao.h"
#if defined(NAU_ENABLE_MGPU)
    #include <wrl/client.h>

    #include "native_ao_algorithm.h"
    #include "nau/diag/logging.h"
namespace nau::render::ao
{
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

    class XeGtaoAlgorithm final : public NativeAlgorithm
    {
    public:
        HRESULT initialize(const std::shared_ptr<mgpu::DeviceContext>& context, UINT width, UINT height, ID3D12Resource* normals, ID3D12Resource* depth, ID3D12Resource* random, ID3D12Resource* output, const ShaderBytecode* shaders, uint32_t shaderCount) override;
        void record(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS constants) override;

    private:
        HRESULT createDescriptors();
        HRESULT createPipelines(const ShaderBytecode* shaders);
        void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER value = {};
            value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            value.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            m_list->ResourceBarrier(1, &value);
        }
        std::shared_ptr<mgpu::DeviceContext> m_context;
        ID3D12Device* m_device = nullptr;
        ID3D12Resource* m_normals = nullptr;
        ID3D12Resource* m_depth = nullptr;
        ID3D12Resource* m_random = nullptr;
        ID3D12Resource* m_output = nullptr;
        mgpu::Resource m_xeDepth, m_xePing, m_xePong, m_xeEdges;
        UINT m_width = 0, m_height = 0, m_srvStride = 0;
        ID3D12GraphicsCommandList* m_list = nullptr;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_xeHeap;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_xeRoot;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_xePso[XE_PASS_COUNT];
    };

    HRESULT XeGtaoAlgorithm::initialize(const std::shared_ptr<mgpu::DeviceContext>& context, UINT width, UINT height, ID3D12Resource* normals, ID3D12Resource* depth, ID3D12Resource* random, ID3D12Resource* output, const ShaderBytecode* shaders, uint32_t shaderCount)
    {
        if (width < 16 || height < 16 || shaderCount != XE_PASS_COUNT || !shaders)
        {
            return E_INVALIDARG;
        }
        for (UINT index = 0; index < shaderCount; ++index)
        {
            if (!shaders[index].data || !shaders[index].size)
            {
                return E_INVALIDARG;
            }
        }
        m_context = context;
        m_device = context->device();
        m_width = width;
        m_height = height;
        m_normals = normals;
        m_depth = depth;
        m_random = random;
        m_output = output;
        m_srvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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

        HRESULT result = S_OK;
        if (SUCCEEDED(result))
        {
            result = m_xeDepth.createTexture(m_context.get(), m_width,
                                             m_height, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 5);
            if (SUCCEEDED(result))
            {
                result = m_xePing.createTexture(m_context.get(), m_width, m_height,
                                                DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            if (SUCCEEDED(result))
            {
                result = m_xePong.createTexture(m_context.get(), m_width, m_height,
                                                DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            if (SUCCEEDED(result))
            {
                result = m_xeEdges.createTexture(m_context.get(), m_width, m_height,
                                                 DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
        }
        if (SUCCEEDED(result))
        {
            result = createDescriptors();
        }
        if (SUCCEEDED(result))
        {
            result = createPipelines(shaders);
        }
        return result;
    }
    HRESULT XeGtaoAlgorithm::createDescriptors()
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
        for (UINT pass = 0; pass < XE_PASS_COUNT; ++pass)
        {
            ID3D12Resource* sources[] = {m_xeDepth.getResource(), m_xeDepth.getResource(),
                                         m_xePing.getResource(), m_xePong.getResource(), m_xePing.getResource()};
            ID3D12Resource* inputs[] = {m_normals, m_depth,
                                        sources[pass], m_xeEdges.getResource(), m_random};
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
            ID3D12Resource* targets[] = {m_xeDepth.getResource(), m_xePing.getResource(),
                                         m_xePong.getResource(), m_xePing.getResource(), m_output};
            for (UINT output = 0; output < XE_UAVS; ++output)
            {
                ID3D12Resource* destination = pass == XE_MAIN && output == 1 ? m_xeEdges.getResource() : targets[pass];
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

    HRESULT XeGtaoAlgorithm::createPipelines(const ShaderBytecode* shaders)
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
        for (UINT pass = 0; pass < XE_PASS_COUNT && SUCCEEDED(result); ++pass)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
            pipeline.pRootSignature = m_xeRoot.Get();
            pipeline.CS = {shaders[pass].data, shaders[pass].size};
            result = m_device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_xePso[pass]));
        }
        return result;
    }

    void XeGtaoAlgorithm::record(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS constants)
    {
        m_list = list;
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12DescriptorHeap* heap[] = {m_xeHeap.Get()};
        m_list->SetDescriptorHeaps(1, heap);
        m_list->SetComputeRootSignature(m_xeRoot.Get());
        m_list->SetComputeRootConstantBufferView(0, constants);
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
        barrier(m_xeDepth.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_PREFILTER, (m_width + 15) / 16, (m_height + 15) / 16);
        barrier(m_xeDepth.getResource(), write, read);
        barrier(m_xePing.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        barrier(m_xeEdges.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_MAIN, (m_width + 7) / 8, (m_height + 7) / 8);
        barrier(m_xePing.getResource(), write, read);
        barrier(m_xeEdges.getResource(), write, read);
        barrier(m_xePong.getResource(), D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_DENOISE, (m_width + 15) / 16, (m_height + 7) / 8);
        barrier(m_xePong.getResource(), write, read);
        barrier(m_xePing.getResource(), read, write);
        dispatch(XE_DENOISE_LAST, (m_width + 15) / 16, (m_height + 7) / 8);
        barrier(m_xePing.getResource(), write, read);
        auto* target = m_output;
        barrier(target, D3D12_RESOURCE_STATE_COMMON, write);
        dispatch(XE_COMPOSITE, (m_width + 7) / 8, (m_height + 7) / 8);
        barrier(target, write, D3D12_RESOURCE_STATE_COMMON);
        barrier(m_xeDepth.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(m_xePing.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(m_xePong.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
        barrier(m_xeEdges.getResource(), read, D3D12_RESOURCE_STATE_COMMON);
    }

    NativeAlgorithm* createXeGtaoAlgorithm()
    {
        return new XeGtaoAlgorithm();
    }
}  // namespace nau::render::ao
#else
namespace nau::render::ao
{
    NativeAlgorithm* createXeGtaoAlgorithm()
    {
        return nullptr;
    }
}  // namespace nau::render::ao
#endif
