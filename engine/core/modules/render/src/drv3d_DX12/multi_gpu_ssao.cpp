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
        TransferChannelHandle rawOutput = nullptr;
        TransferTicket pendingOutput = 0;
        TransferTicket pendingRaw = 0;
        bool reconstructNormals = false;

        ~SsaoProducer()
        {
            // Flush primary signals/waits, then finish copies before releasing endpoints and their allocator.
            destroy_transfer_channel(output);
            destroy_transfer_channel(rawOutput);
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
            pass.profiler = desc.profiler;
            pass.normalShader = {desc.normalBytecode, desc.normalBytecodeSize};
            producer->reconstructNormals = desc.normalBytecode != nullptr;
            result = producer->pass.init(pass);
        }
        if (SUCCEEDED(result))
        {
            TransferChannelDesc channel;
            channel.width = desc.width;
            channel.height = desc.height;
            channel.slotCount = 3;
            channel.direction = TransferDirection::PrimaryToSecondary;
            if (!producer->reconstructNormals)
            {
                channel.format = TEXFMT_G16R16F;
                channel.name = "SSAO normals GPU 0 -> GPU 1";
                channel.profiler = desc.profiler ? desc.profiler->createTransferProfiler("ssao_normals", true) : nullptr;
                producer->normals = create_transfer_channel(channel);
            }
            channel.format = desc.depthIsFloat ? TEXFMT_R32F : TEXFMT_DEPTH32;
            channel.name = "SSAO depth GPU 0 -> GPU 1";
            channel.profiler = desc.profiler ? desc.profiler->createTransferProfiler("ssao_depth", true) : nullptr;
            producer->depth = create_transfer_channel(channel);
            channel.direction = TransferDirection::SecondaryToPrimary;
            channel.format = TEXFMT_R8;
            channel.name = "SSAO result GPU 1 -> GPU 0";
            channel.profiler = desc.profiler ? desc.profiler->createTransferProfiler("ssao_result", false) : nullptr;
            producer->output = create_transfer_channel(channel);
            result = (producer->reconstructNormals || producer->normals) && producer->depth && producer->output ? S_OK : E_FAIL;
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
        return producer && (producer->pendingOutput != 0 || producer->pass.canSubmit() == DXGI_ERROR_WAS_STILL_DRAWING);
    }

    SsaoResult submitSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, bool captureRaw)
    {
        if (!producer || (!producer->reconstructNormals && !normals) || !depth || !is_multi_gpu_available())
        {
            return SsaoResult::failed;
        }
        if (producer->pendingOutput != 0)
        {
            return SsaoResult::busy;
        }
        HRESULT result = producer->pass.canSubmit();
        if (result == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return SsaoResult::busy;
        }
        if (SUCCEEDED(result))
        {
            const TransferTicket normalTicket = producer->reconstructNormals ? 0 : send_from_primary(producer->normals, normals);
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
            if ((!producer->reconstructNormals && !normalTicket) || !depthTicket)
            {
                return SsaoResult::busy;
            }
            if ((!producer->reconstructNormals && !normalReady.nativeFence) || !depthReady.nativeFence)
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
            if (captureRaw && !producer->rawOutput)
            {
                const D3D12_RESOURCE_DESC size = producer->pass.getNormals()->GetDesc();
                TransferChannelDesc channel;
                channel.width = static_cast<uint32_t>(size.Width);
                channel.height = size.Height;
                channel.format = TEXFMT_R32F;
                channel.slotCount = 3;
                channel.direction = TransferDirection::SecondaryToPrimary;
                channel.name = "SSAO raw validation GPU 1 -> GPU 0";
                producer->rawOutput = create_transfer_channel(channel);
                if (!producer->rawOutput)
                {
                    report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
                    return SsaoResult::failed;
                }
            }
            const drv3d_dx12::CrossAdapterSyncPoint rendered = producer->pass.getReadyPoint();
            const SecondarySyncPoint ready = {rendered.fence, rendered.value};
            for (bool raw : {true, false})
            {
                if (raw && !captureRaw)
                {
                    continue;
                }
                const TransferChannelHandle channel = raw ? producer->rawOutput : producer->output;
                const SecondarySend sent = send_from_secondary(channel, producer->pass.getResult(raw), ready);
                if (sent.ticket == 0)
                {
                    // Inputs and the pass are already committed. A missing output is a failed job.
                    result = E_FAIL;
                    break;
                }
                result = producer->pass.releaseOutput({static_cast<ID3D12Fence*>(sent.sourceReleased.nativeFence), sent.sourceReleased.value}, raw);
                if (FAILED(result))
                {
                    break;
                }
                if (raw)
                {
                    producer->pendingRaw = sent.ticket;
                }
                else
                {
                    producer->pendingOutput = sent.ticket;
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

    namespace
    {
        SsaoResult receiveSsao(SsaoProducer* producer, BaseTexture* destination, BaseTexture* rawDestination, bool poll)
        {
            if (!producer || !destination || !is_multi_gpu_available())
            {
                return SsaoResult::failed;
            }
            if (producer->pendingOutput == 0)
            {
                return SsaoResult::busy;
            }
            if (poll && (!is_transfer_ready(producer->output, producer->pendingOutput) ||
                         (producer->pendingRaw != 0 && !is_transfer_ready(producer->rawOutput, producer->pendingRaw))))
            {
                return SsaoResult::busy;
            }
            if ((producer->pendingRaw != 0 && (!rawDestination || !receive_on_primary(producer->rawOutput, producer->pendingRaw, rawDestination))) ||
                !receive_on_primary(producer->output, producer->pendingOutput, destination))
            {
                report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
                return SsaoResult::failed;
            }
            producer->pendingOutput = 0;
            producer->pendingRaw = 0;
            return SsaoResult::submitted;
        }
    }  // namespace

    SsaoResult tryReceiveSsao(SsaoProducer* producer, BaseTexture* destination, BaseTexture* rawDestination)
    {
        return receiveSsao(producer, destination, rawDestination, true);
    }

    SsaoResult renderSsao(SsaoProducer* producer, const SsaoConstants& constants, bool blur, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, BaseTexture* rawDestination)
    {
        if (!destination)
        {
            return SsaoResult::failed;
        }
        const SsaoResult result = submitSsao(producer, constants, blur, normals, depth, rawDestination != nullptr);
        return result == SsaoResult::submitted ? receiveSsao(producer, destination, rawDestination, false) : result;
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
