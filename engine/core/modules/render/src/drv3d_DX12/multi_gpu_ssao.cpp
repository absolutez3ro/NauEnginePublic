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
        drv3d_dx12::DeviceSsaoPass pass;
        TransferChannelHandle normals = nullptr;
        TransferChannelHandle depth = nullptr;
        TransferChannelHandle output = nullptr;

        ~SsaoProducer()
        {
            // Flush primary signals/waits, then finish copies before releasing endpoints and their allocator.
            destroy_transfer_channel(output);
            destroy_transfer_channel(normals);
            destroy_transfer_channel(depth);
            const HRESULT result = pass.reset();
            if (FAILED(result))
            {
                NAU_LOG_WARNING("SSAO: GPU 1 pass retired during shutdown, HRESULT {:#x}", static_cast<uint32_t>(result));
            }
        }
    };

    SsaoProducer* createSsaoProducer(const SsaoProducerDesc& desc)
    {
        if (!is_multi_gpu_available())
        {
            return nullptr;
        }
        const auto allocator = eastl::make_shared<drv3d_dx12::NauDeviceResourceAllocator>();
        HRESULT result = allocator->init(static_cast<ID3D12Device*>(get_device(SECONDARY_GPU)));
        auto producer = eastl::make_unique<SsaoProducer>();
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
            channel.direction = TransferDirection::PrimaryToSecondary;
            channel.format = TEXFMT_A32B32G32R32F;
            channel.name = "SSAO normals GPU 0 -> GPU 1";
            producer->normals = create_transfer_channel(channel);
            channel.format = TEXFMT_DEPTH32;
            channel.name = "SSAO depth GPU 0 -> GPU 1";
            producer->depth = create_transfer_channel(channel);
            channel.direction = TransferDirection::SecondaryToPrimary;
            channel.format = TEXFMT_R32F;
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

    SsaoResult renderSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, BaseTexture* rawDestination)
    {
        if (!producer || !normals || !depth || !destination || !is_multi_gpu_available())
        {
            return SsaoResult::failed;
        }
        HRESULT result = producer->pass.canSubmit();
        if (result == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return SsaoResult::busy;
        }
        if (SUCCEEDED(result))
        {
            const TransferTicket normalTicket = send_from_primary(producer->normals, normals);
            const TransferTicket depthTicket = send_from_primary(producer->depth, depth);
            // Receive whichever sends succeeded, even if its partner was dropped. This releases both channel slots.
            const drv3d_dx12::CrossAdapterSyncPoint free = producer->pass.getReadyPoint();
            const SecondarySyncPoint destinationFree = {free.fence, free.value};
            const SecondarySyncPoint normalReady = normalTicket
                                                       ? receive_on_secondary(producer->normals, normalTicket, producer->pass.getNormals(), destinationFree)
                                                       : SecondarySyncPoint{};
            const SecondarySyncPoint depthReady = depthTicket
                                                      ? receive_on_secondary(producer->depth, depthTicket, producer->pass.getDepth(), destinationFree)
                                                      : SecondarySyncPoint{};
            if (!normalTicket || !depthTicket)
            {
                return SsaoResult::busy;
            }
            if (!normalReady.nativeFence || !depthReady.nativeFence)
            {
                result = E_FAIL;
            }
            else
            {
                result = producer->pass.execute(constants, blur,
                                                {static_cast<ID3D12Fence*>(normalReady.nativeFence), normalReady.value},
                                                {static_cast<ID3D12Fence*>(depthReady.nativeFence), depthReady.value});
            }
        }
        if (SUCCEEDED(result))
        {
            const drv3d_dx12::CrossAdapterSyncPoint rendered = producer->pass.getReadyPoint();
            const SecondarySyncPoint ready = {rendered.fence, rendered.value};
            for (bool raw : {true, false})
            {
                BaseTexture* target = raw ? rawDestination : destination;
                if (!target)
                {
                    continue;
                }
                const SecondarySend sent = send_from_secondary(producer->output, producer->pass.getResult(raw), ready);
                if (sent.ticket == 0)
                {
                    return SsaoResult::busy;
                }
                result = producer->pass.releaseOutput({static_cast<ID3D12Fence*>(sent.sourceReleased.nativeFence), sent.sourceReleased.value});
                if (FAILED(result))
                {
                    break;
                }
                if (!receive_on_primary(producer->output, sent.ticket, target))
                {
                    result = E_FAIL;
                    break;
                }
            }
            if (SUCCEEDED(result))
            {
                return SsaoResult::submitted;
            }
        }
        if (result == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return SsaoResult::busy;
        }
        report_secondary_gpu_failure(static_cast<uint32_t>(result));
        return SsaoResult::failed;
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
