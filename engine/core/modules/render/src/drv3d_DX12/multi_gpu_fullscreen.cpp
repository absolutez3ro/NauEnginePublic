// multi_gpu_fullscreen.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/multi_gpu_fullscreen.h"

#include "device_fullscreen_pass.h"
#include "device_ssao_pass.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau_device_resource_allocator.h"

namespace d3d::mgpu
{
    struct FullscreenProducer
    {
        drv3d_dx12::DeviceFullscreenPass pass;
        TransferChannelHandle channel = nullptr;

        ~FullscreenProducer()
        {
            // Flush engine waits before draining the channel. The pass retains sourceReleased fences until done.
            destroy_transfer_channel(channel);
            const HRESULT result = pass.reset();
            if (FAILED(result))
            {
                NAU_LOG_WARNING("MG-12: producer retired during shutdown, HRESULT {:#x}", static_cast<uint32_t>(result));
            }
        }
    };

    FullscreenProducer* createFullscreenProducer(const FullscreenProducerDesc& desc)
    {
        if (!is_multi_gpu_available())
        {
            return nullptr;
        }
        const auto allocator = eastl::make_shared<drv3d_dx12::NauDeviceResourceAllocator>();
        HRESULT result = allocator->init(static_cast<ID3D12Device*>(get_device(SECONDARY_GPU)));
        auto producer = eastl::make_unique<FullscreenProducer>();
        if (SUCCEEDED(result))
        {
            drv3d_dx12::DeviceFullscreenPassDesc passDesc;
            passDesc.allocator = allocator;
            passDesc.width = desc.width;
            passDesc.height = desc.height;
            passDesc.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            passDesc.pixelShader = {desc.pixelBytecode, desc.pixelBytecodeSize};
            result = producer->pass.init(passDesc);
        }
        if (SUCCEEDED(result))
        {
            TransferChannelDesc channelDesc;
            channelDesc.width = desc.width;
            channelDesc.height = desc.height;
            channelDesc.format = TEXFMT_R8G8B8A8;
            channelDesc.name = "MG-12 animated fullscreen";
            producer->channel = create_transfer_channel(channelDesc);
            result = producer->channel ? S_OK : E_FAIL;
        }
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return nullptr;
        }
        return producer.release();
    }

    FullscreenResult renderFullscreen(FullscreenProducer* producer, const float* constants, BaseTexture* destination)
    {
        if (!producer || !constants || !destination || !is_multi_gpu_available())
        {
            return FullscreenResult::failed;
        }
        producer->pass.setColor({constants[0], constants[1], constants[2], constants[3]});
        HRESULT result = producer->pass.execute(false);
        if (SUCCEEDED(result))
        {
            const SecondarySend sent = send_from_secondary(producer->channel, producer->pass.getRenderTarget(),
                                                           {producer->pass.getFence(), producer->pass.getLastSubmittedValue()});
            if (sent.ticket == 0)
            {
                result = producer->pass.getDevice()->GetDeviceRemovedReason();
                if (SUCCEEDED(result))
                {
                    return FullscreenResult::busy;
                }
            }
            else
            {
                // Register the consumer even if the receive drops: GPU 1 must never overwrite a copy source in use.
                result = producer->pass.waitForExternalWork(static_cast<ID3D12Fence*>(sent.sourceReleased.nativeFence),
                                                            sent.sourceReleased.value);
                if (SUCCEEDED(result))
                {
                    return receive_on_primary(producer->channel, sent.ticket, destination) ? FullscreenResult::submitted
                                                                                           : FullscreenResult::busy;
                }
            }
        }
        report_secondary_gpu_failure(static_cast<uint32_t>(result));
        return FullscreenResult::failed;
    }

    void destroyFullscreenProducer(FullscreenProducer* producer)
    {
        delete producer;
        collectFullscreenResources();
    }

    void collectFullscreenResources()
    {
        drv3d_dx12::DeviceFullscreenPass::collectRetiredResources();
        drv3d_dx12::DeviceSsaoPass::collectRetiredResources();
    }
}  // namespace d3d::mgpu
