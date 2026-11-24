// main.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include <cstdio>
#include <cstring>

#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GDeviceFactory.h"
#include "nau/mgpu/bridge/device_context.h"
#include "nau/mgpu/mailbox/mailbox_job.h"
#include "nau/mgpu/mailbox/mailbox_state.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    using nau::mgpu::DeviceContext;
    using nau::mgpu::Resource;

    bool check(HRESULT result)
    {
        if (FAILED(result))
        {
            std::printf("HRESULT=%08x\n", static_cast<unsigned>(result));
        }
        return SUCCEEDED(result);
    }

    void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, 0, before, after};
        list->ResourceBarrier(1, &barrier);
    }

    bool generation(const std::shared_ptr<DeviceContext>& primary, const std::shared_ptr<DeviceContext>& secondary, UINT width)
    {
        nau::mgpu::MailboxState mailbox;
        nau::mgpu::MailboxDescription layout;
        layout.inputCount = 1;
        layout.inputs[0] = layout.output = {width, 1, DXGI_FORMAT_R8_UNORM};
        std::puts("init mailbox");
        if (!check(mailbox.init(primary, secondary, layout)))
        {
            return false;
        }
        std::puts("create local resources");
        Resource upload, output, readback;
        if (!check(upload.createBuffer(secondary.get(), 2048, D3D12_HEAP_TYPE_UPLOAD)) ||
            !check(output.createTexture(primary.get(), width, 1, DXGI_FORMAT_R8_UNORM)) ||
            !check(readback.createBuffer(primary.get(), 1024, D3D12_HEAP_TYPE_READBACK)))
            return false;
        std::puts("map upload");
        uint8_t* bytes = nullptr;
        const D3D12_RANGE noRead = {0, 0};
        if (!check(upload.getResource()->Map(0, &noRead, reinterpret_cast<void**>(&bytes))))
        {
            return false;
        }
        ComPtr<ID3D12CommandAllocator> allocators[2];
        ComPtr<ID3D12GraphicsCommandList> payload, publication, receive;
        for (UINT side = 0; side < 2; ++side)
        {
            if (!check((side ? secondary : primary)->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[side]))))
                return false;
        }
        if (!check(primary->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&receive))) ||
            !check(secondary->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[1].Get(), nullptr, IID_PPV_ARGS(&payload))) ||
            !check(payload->Close()) ||
            !check(secondary->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[1].Get(), nullptr, IID_PPV_ARGS(&publication))))
            return false;
        receive->Close();
        publication->Close();
        std::puts("start transfers");
        for (uint32_t frame = 1; frame <= 40; ++frame)
        {
            if (!mailbox.poll())
            {
                return false;
            }
            const auto reservation = mailbox.reserve();
            if (!reservation.accepted)
            {
                return false;
            }
            if (!check(primary->graphicsQueue()->Signal(mailbox.inputsReady.getFence(nau::mgpu::CrossAdapterFence::Side::SIGNALER), reservation.inputValue)))
            {
                return false;
            }
            allocators[1]->Reset();
            payload->Reset(allocators[1].Get(), nullptr);

            std::memset(bytes, static_cast<int>(frame), width);
            auto* target = mailbox.resources.output(reservation.output, 1);
            transition(payload.Get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
            source.pResource = upload.getResource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, width, 1, 1, 256};
            destination.pResource = target;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            payload->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            transition(payload.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            nau::mgpu::MailboxJob job;
            job.inputsReady = {mailbox.inputsReady.getFence(nau::mgpu::CrossAdapterFence::Side::WAITER), reservation.inputValue};
            job.completion = &mailbox.resultsReady;
            job.frameId = mailbox.resources.outputId(reservation.output, 1);
            job.mailbox = mailbox.resources.mailboxSecondary();
            job.outputSlot = reservation.output;
            nau::mgpu::recordMailboxId(payload.Get(), job, frame, upload.getResource(), bytes, 512);
            if (!check(payload->Close()) || !check(publication->Reset(allocators[1].Get(), nullptr))) return false;
            nau::mgpu::recordMailboxPublication(publication.Get(), job, frame, upload.getResource(), bytes, 768);
            if (!check(publication->Close()))
            {
                return false;
            }
            nau::mgpu::MailboxCompletion completion;
            bool submitted = false;
            if (!check(nau::mgpu::submitMailboxJob(secondary->graphicsQueue(), payload.Get(), publication.Get(), job, completion, submitted)))
            {
                return false;
            }
            mailbox.commit(reservation, completion.dataReady, completion.published, frame);
            if (!check(mailbox.resultsReady.waitOnCpu(completion.published, 10000)))
            {
                return false;
            }
            // Receive twice: the second receive must retain the same payload/source pair without new work.
            for (UINT repeat = 0; repeat < 2; ++repeat)
            {
                allocators[0]->Reset();
                receive->Reset(allocators[0].Get(), nullptr);
                auto desc = mailbox.receive(frame, false);
                desc.readback = readback.getResource();
                desc.readbackOffset = 512;
                transition(receive.Get(), output.getResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                nau::mgpu::recordMailboxReceive(receive.Get(), output.getResource(), desc);
                transition(receive.Get(), output.getResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                source = {};
                destination = {};
                source.pResource = output.getResource();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.pResource = readback.getResource();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, width, 1, 1, 256};
                receive->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                transition(receive.Get(), output.getResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (!check(receive->Close()))
                {
                    return false;
                }
                ID3D12CommandList* command = receive.Get();
                primary->graphicsQueue()->ExecuteCommandLists(1, &command);
                const uint64_t retired = mailbox.inputsReady.enqueueSignal(primary->graphicsQueue());
                if (!retired || !check(mailbox.inputsReady.waitOnCpu(retired, 10000)))
                {
                    return false;
                }
                const D3D12_RANGE range = {0, 516};
                const uint8_t* result = nullptr;
                if (!check(readback.getResource()->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&result)))))
                {
                    return false;
                }
                uint32_t id = 0;
                std::memcpy(&id, result + 512, sizeof(id));
                bool matches = id == frame;
                for (UINT pixel = 0; pixel < width; ++pixel)
                {
                    matches = matches && result[pixel] == frame;
                }
                readback.getResource()->Unmap(0, &noRead);
                if (!matches)
                {
                    std::printf("Mismatch frame=%u id=%u\n", frame, id);
                    return false;
                }
            }
        }
        upload.getResource()->Unmap(0, nullptr);
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    namespace graphics = nau::PEPEngine::Graphics;
    graphics::GDeviceFactory::SetDebugLayerEnabled(false);
    auto& devices = graphics::GDeviceFactory::GetAllDevices();
    if (devices.empty())
    {
        return 2;
    }
    auto primary = std::make_shared<DeviceContext>();
    auto secondary = std::make_shared<DeviceContext>();
    if (!check(primary->attach(devices.front()->GetDXDevice().Get())) ||
        !check(secondary->selectSecondary(primary->device()->GetAdapterLuid(), nullptr, argc > 1 && std::strcmp(argv[1], "--warp") == 0)))
        return 2;
    bool passed = true;
    for (UINT width : {128u, 256u, 128u})
    {
        passed = generation(primary, secondary, width) && passed;
        std::printf("Generation width=%u: %s\n", width, passed ? "PASS" : "FAIL");
        if (!passed)
        {
            break;
        }
    }
    nau::mgpu::shutdownDevices();
    std::printf("Mailbox payload/ID, slot reuse, retained result and recreation: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
