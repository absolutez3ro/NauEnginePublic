// multi_gpu_ssao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/multi_gpu_ssao.h"

#include "device_ssao_pass.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau_device_resource_allocator.h"

namespace d3d::mgpu
{
    struct SsaoProducer
    {
        SsaoProducer() = default;
        SsaoProducer(const SsaoProducer&) = delete;
        SsaoProducer(SsaoProducer&&) = delete;
        SsaoProducer& operator=(const SsaoProducer&) = delete;
        SsaoProducer& operator=(SsaoProducer&&) = delete;

        drv3d_dx12::DeviceSsaoPass pass;
        TransferChannelHandle normals = nullptr;
        TransferChannelHandle depth = nullptr;
        TransferChannelHandle output = nullptr;
        bool reconstructNormals = false;

        ~SsaoProducer()
        {
            // Endpoints owned by the technique remain alive until all copy queues drain.
            destroy_transfer_channel(output);
            destroy_transfer_channel(normals);
            destroy_transfer_channel(depth);
            const HRESULT result = pass.reset();
            if (FAILED(result))
            {
                NAU_LOG_WARNING("SSAO: GPU 1 resources retired, HRESULT {:#x}", static_cast<uint32_t>(result));
            }
        }
    };

    SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc)
    {
        if (!is_multi_gpu_available())
        {
            return nullptr;
        }
        const eastl::shared_ptr<drv3d_dx12::NauDeviceResourceAllocator> allocator = eastl::make_shared<drv3d_dx12::NauDeviceResourceAllocator>();
        HRESULT result = allocator->init(static_cast<ID3D12Device*>(get_device(SECONDARY_GPU)));
        eastl::unique_ptr<SsaoProducer> producer = eastl::make_unique<SsaoProducer>();
        if (SUCCEEDED(result))
        {
            drv3d_dx12::DeviceSsaoPassDesc pass;
            pass.allocator = allocator;
            pass.width = desc.width;
            pass.height = desc.height;
            pass.inputWidth = desc.inputWidth;
            pass.inputHeight = desc.inputHeight;
            pass.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            pass.rawShader = {desc.rawBytecode, desc.rawBytecodeSize};
            pass.filterShader = {desc.filterBytecode, desc.filterBytecodeSize};
            pass.reconstructNormals = producer->reconstructNormals = desc.reconstructNormals;
            pass.packedNormals = desc.packedNormals;
            pass.linearDepth16 = desc.linearDepth16;
            result = producer->pass.init(pass);
        }
        if (SUCCEEDED(result))
        {
            TransferChannelDesc channel;
            channel.width = desc.inputWidth ? desc.inputWidth : desc.width;
            channel.height = desc.inputHeight ? desc.inputHeight : desc.height;
            channel.slotCount = 3;
            channel.direction = TransferDirection::primaryToSecondary;
            channel.format = desc.packedNormals ? TEXFMT_R8G8 : TEXFMT_G16R16F;
            channel.name = "SSAO prepass normals GPU 0 -> GPU 1";
            if (!desc.reconstructNormals) producer->normals = create_transfer_channel(channel);
            channel.format = desc.linearDepth16 ? TEXFMT_R16F : TEXFMT_R32F;
            channel.name = "SSAO depth GPU 0 -> GPU 1";
            producer->depth = create_transfer_channel(channel);
            channel.width = desc.width;
            channel.height = desc.height;
            channel.direction = TransferDirection::secondaryToPrimary;
            channel.format = TEXFMT_R8;
            channel.name = "SSAO result GPU 1 -> GPU 0";
            producer->output = create_transfer_channel(channel);
            result = (desc.reconstructNormals || producer->normals) && producer->depth && producer->output ? S_OK : E_FAIL;
        }
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return nullptr;
        }
        return producer.release();
    }

    PrimarySyncPoint submitSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination)
    {
        if (!producer || !normals || !depth || !destination || !is_multi_gpu_available())
        {
            return {};
        }
        const PrimarySend normalSent = producer->reconstructNormals ? PrimarySend{} : send_from_primary_deferred(producer->normals, normals);
        const PrimarySend depthSent = send_from_primary_deferred(producer->depth, depth);
        const drv3d_dx12::CrossAdapterSyncPoint free = producer->pass.getReadyPoint();
        const SecondarySyncPoint destinationFree = {free.fence, free.value};
        // Consume partial sends as well, so failure recovery can drain every issued copy.
        const SecondarySyncPoint normalReady = normalSent.ticket
                                                   ? receive_on_secondary(producer->normals, normalSent.ticket, producer->pass.getNormals(), destinationFree)
                                                   : SecondarySyncPoint{};
        const SecondarySyncPoint depthReady = depthSent.ticket
                                                  ? receive_on_secondary(producer->depth, depthSent.ticket, producer->pass.getDepth(), destinationFree)
                                                  : SecondarySyncPoint{};
        HRESULT result = (producer->reconstructNormals || normalReady.nativeFence) && depthReady.nativeFence ? S_OK : E_FAIL;
        if (SUCCEEDED(result))
        {
            result = producer->pass.execute(constants,
                                            {static_cast<ID3D12Fence*>(normalReady.nativeFence), normalReady.value},
                                            {static_cast<ID3D12Fence*>(depthReady.nativeFence), depthReady.value});
        }
        if (SUCCEEDED(result))
        {
            const drv3d_dx12::CrossAdapterSyncPoint rendered = producer->pass.getReadyPoint();
            const SecondarySend sent = send_from_secondary(producer->output, producer->pass.getResult(), {rendered.fence, rendered.value});
            result = sent.ticket ? producer->pass.releaseOutput({static_cast<ID3D12Fence*>(sent.sourceReleased.nativeFence), sent.sourceReleased.value}) : E_FAIL;
            if (SUCCEEDED(result))
            {
                // The copy queue waits for AO on GPU 1. Graphics waits only later, at ssao_receive.
                const PrimarySyncPoint receiving = begin_receive_on_primary(producer->output, sent.ticket, destination);
                if (receiving.nativeFence)
                {
                    return receiving;
                }
                result = E_FAIL;
            }
        }
        report_secondary_gpu_failure(static_cast<uint32_t>(result));
        return {};
    }

    void destroySsaoProducer(SsaoProducer* producer)
    {
        delete producer;
        collectSsaoResources();
    }

    void collectSsaoResources()
    {
        drv3d_dx12::DeviceSsaoPass::collectRetiredResources();
    }
}  // namespace d3d::mgpu
