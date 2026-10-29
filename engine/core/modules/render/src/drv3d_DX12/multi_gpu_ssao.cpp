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
        PrimarySyncPoint normalsReleased;
        PrimarySyncPoint depthReleased;
        PrimarySyncPoint receiving;
        BaseTexture* destination = nullptr;
        TransferTicket pendingOutput = 0;

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
            pass.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            pass.rawShader = {desc.rawBytecode, desc.rawBytecodeSize};
            pass.blurShader = {desc.blurBytecode, desc.blurBytecodeSize};
            result = producer->pass.init(pass);
        }
        if (SUCCEEDED(result))
        {
            TransferChannelDesc channel;
            channel.width = desc.width;
            channel.height = desc.height;
            channel.slotCount = 3;
            channel.direction = TransferDirection::primaryToSecondary;
            channel.format = TEXFMT_G32R32F;
            channel.name = "SSAO G-buffer normals GPU 0 -> GPU 1";
            producer->normals = create_transfer_channel(channel);
            channel.format = TEXFMT_DEPTH32;
            channel.name = "SSAO depth GPU 0 -> GPU 1";
            producer->depth = create_transfer_channel(channel);
            channel.direction = TransferDirection::secondaryToPrimary;
            channel.format = TEXFMT_R8;
            channel.name = "SSAO result GPU 1 -> GPU 0";
            producer->output = create_transfer_channel(channel);
            result = producer->normals && producer->depth && producer->output ? S_OK : E_FAIL;
        }
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return nullptr;
        }
        return producer.release();
    }

    bool isSsaoBusy(const SsaoProducer* producer)
    {
        if (!producer)
        {
            return false;
        }
        const HRESULT status = producer->pass.canSubmit();
        if (FAILED(status) && status != DXGI_ERROR_WAS_STILL_DRAWING)
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(status));
        }
        return producer && (producer->pendingOutput != 0 ||
                            !is_primary_transfer_complete(producer->normalsReleased) ||
                            !is_primary_transfer_complete(producer->depthReleased) ||
                            status != S_OK);
    }

    SsaoResult submitSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth)
    {
        if (!producer || !normals || !depth || !is_multi_gpu_available())
        {
            return SsaoResult::failed;
        }
        if (isSsaoBusy(producer))
        {
            return SsaoResult::busy;
        }
        const PrimarySend normalSent = send_from_primary_deferred(producer->normals, normals);
        const PrimarySend depthSent = send_from_primary_deferred(producer->depth, depth);
        producer->normalsReleased = normalSent.sourceReleased;
        producer->depthReleased = depthSent.sourceReleased;
        const drv3d_dx12::CrossAdapterSyncPoint free = producer->pass.getReadyPoint();
        const SecondarySyncPoint destinationFree = {free.fence, free.value};
        // Consume either successful send, including a partial pair, to release every shared slot.
        const SecondarySyncPoint normalReady = normalSent.ticket
                                                   ? receive_on_secondary(producer->normals, normalSent.ticket, producer->pass.getNormals(), destinationFree)
                                                   : SecondarySyncPoint{};
        const SecondarySyncPoint depthReady = depthSent.ticket
                                                  ? receive_on_secondary(producer->depth, depthSent.ticket, producer->pass.getDepth(), destinationFree)
                                                  : SecondarySyncPoint{};
        HRESULT result = S_OK;
        if ((normalSent.ticket && !normalReady.nativeFence) || (depthSent.ticket && !depthReady.nativeFence))
        {
            result = E_FAIL;
        }
        else if (!normalSent.ticket || !depthSent.ticket)
        {
            return SsaoResult::busy;
        }
        else
        {
            result = producer->pass.execute(constants, blur,
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
                producer->pendingOutput = sent.ticket;
                return SsaoResult::submitted;
            }
        }
        report_secondary_gpu_failure(static_cast<uint32_t>(result));
        return SsaoResult::failed;
    }

    SsaoResult tryReceiveSsao(SsaoProducer* producer, BaseTexture* destination)
    {
        if (!producer || !destination || !is_multi_gpu_available())
        {
            return SsaoResult::failed;
        }
        if (!producer->pendingOutput)
        {
            return SsaoResult::busy;
        }
        if (!producer->receiving.nativeFence)
        {
            if (!is_transfer_ready(producer->output, producer->pendingOutput))
            {
                return SsaoResult::busy;
            }
            producer->receiving = begin_receive_on_primary(producer->output, producer->pendingOutput, destination);
            producer->destination = destination;
            if (!producer->receiving.nativeFence)
            {
                report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
                return SsaoResult::failed;
            }
        }
        if (producer->destination != destination)
        {
            return SsaoResult::failed;
        }
        if (!is_primary_transfer_complete(producer->receiving))
        {
            return SsaoResult::busy;
        }
        producer->pendingOutput = 0;
        producer->receiving = {};
        producer->destination = nullptr;
        return SsaoResult::submitted;
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
