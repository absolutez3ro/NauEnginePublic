// multi_gpu_ssao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/multi_gpu_ssao.h"

#include "device_ssao_pass.h"
#include "mailbox_ssao_state.h"
#include "device.h"
#include "texture.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau_device_resource_allocator.h"
#include "multi_gpu_device.h"

namespace d3d::mgpu
{
    namespace
    {

        bool usePackedSoftwareDepth(ID3D12Device* device)
        {
            // WARP benefits from grouping its shared-memory reads.
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc = {};
            return SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
                   SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
                   SUCCEEDED(adapter->GetDesc1(&desc)) && drv3d_dx12::is_software_device(desc);
        }
    }

    struct SsaoProducer
    {
        SsaoProducer() = default;
        SsaoProducer(const SsaoProducer&) = delete;
        SsaoProducer(SsaoProducer&&) = delete;
        SsaoProducer& operator=(const SsaoProducer&) = delete;
        SsaoProducer& operator=(SsaoProducer&&) = delete;

        drv3d_dx12::DeviceSsaoPass pass;
        eastl::shared_ptr<drv3d_dx12::MailboxSsaoState> mailbox;
        bool reconstructNormals = false;
        bool measureTimings = false;
        struct PrimaryTiming
        {
            void* begin = nullptr;
            void* published = nullptr;
            void* received = nullptr;
            void* receiveBegin = nullptr;
            uint64_t frame = 0;
            bool pending = false;
        };
        PrimaryTiming primaryQueries[16];
        uint64_t primaryFrame = 0;
        float publishMilliseconds = -1.f;
        float receiveMilliseconds = -1.f;

        void pollPrimaryTiming()
        {
            if (!mailbox || !measureTimings)
            {
                return;
            }
            uint64_t frequency = 0;
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &frequency, nullptr, nullptr);
            for (auto& query : primaryQueries)
            {
                uint64_t begin = 0, published = 0, received = 0, receiveBegin = 0;
                if (!query.pending ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.begin, &begin, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.published, &published, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.received, &received, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.receiveBegin, &receiveBegin, nullptr))
                {
                    continue;
                }
                if (frequency && query.frame > primaryFrame && published >= begin && received >= published)
                {
                    primaryFrame = query.frame;
                    publishMilliseconds = static_cast<float>(1000.0 * static_cast<double>(published - begin) / frequency);
                    receiveMilliseconds = received >= receiveBegin ? static_cast<float>(1000.0 * static_cast<double>(received - receiveBegin) / frequency) : -1.f;
                }
                query.pending = false;
            }
        }

        ~SsaoProducer()
        {
            if (mailbox)
            {
                drv3d_dx12::get_device().getContext().wait();
                const HRESULT result = pass.reset();
                if (FAILED(result))
                {
                    NAU_LOG_WARNING("AO mailbox: GPU1 drain failed, HRESULT {:#x}", static_cast<uint32_t>(result));
                }
                mailbox->inputsReady.waitOnCpu(mailbox->inputsReady.issuedValue(), 10000);
                mailbox->resultsReady.waitOnCpu(mailbox->resultsReady.issuedValue(), 10000);
            }
            for (auto& query : primaryQueries)
            {
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.begin, nullptr, nullptr);
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.published, nullptr, nullptr);
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.received, nullptr, nullptr);
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.receiveBegin, nullptr, nullptr);
            }
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
        producer->measureTimings = desc.measureTimings;
        if (SUCCEEDED(result))
        {
            drv3d_dx12::DeviceSsaoPassDesc pass;
            pass.allocator = allocator;
            pass.width = desc.width;
            pass.height = desc.height;
            pass.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            pass.rawShader = {desc.rawBytecode, desc.rawBytecodeSize};
            pass.rawComputeShader = {desc.rawComputeBytecode, desc.rawComputeBytecodeSize};
            pass.xePrefilterShader = {desc.xePrefilterBytecode, desc.xePrefilterBytecodeSize};
            pass.xeMainShader = {desc.xeMainBytecode, desc.xeMainBytecodeSize};
            pass.xeDenoiseShader = {desc.xeDenoiseBytecode, desc.xeDenoiseBytecodeSize};
            pass.xeDenoiseLastShader = {desc.xeDenoiseLastBytecode, desc.xeDenoiseLastBytecodeSize};
            pass.xeCompositeShader = {desc.xeCompositeBytecode, desc.xeCompositeBytecodeSize};
            pass.reconstructNormals = producer->reconstructNormals = desc.reconstructNormals;
            pass.measureTimings = desc.measureTimings;
            pass.packedFrameworkDepth = usePackedSoftwareDepth(allocator->getDevice());
            pass.packedFrameworkNormals = pass.packedFrameworkDepth && !desc.reconstructNormals;
            for (uint32_t attempt = 0; attempt < 2; ++attempt)
            {
                result = producer->pass.init(pass);
                if (SUCCEEDED(result))
                {
                    producer->mailbox = eastl::make_shared<drv3d_dx12::MailboxSsaoState>();
                    result = producer->mailbox->init(static_cast<ID3D12Device*>(get_device(PRIMARY_GPU)),
                                                     allocator->getDevice(), pass);
                    if (SUCCEEDED(result))
                    {
                        producer->pass.retainMailboxResources(producer->mailbox);
                    }
                }
                if (SUCCEEDED(result) || !pass.packedFrameworkDepth)
                {
                    break;
                }
                producer->mailbox.reset();
                producer->pass.reset();
                pass.packedFrameworkDepth = false;
                pass.packedFrameworkNormals = false;
                NAU_LOG_WARNING("SSAO: packed software depth unavailable, HRESULT {:#x}; using ordinary shared depth", static_cast<uint32_t>(result));
            }
        }
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return nullptr;
        }
        return producer.release();
    }

    SsaoGpuTiming pollSsaoTiming(SsaoProducer* producer)
    {
        if (!producer)
        {
            return {};
        }
        producer->pollPrimaryTiming();
        if (producer->mailbox)
        {
            producer->mailbox->poll();
        }
        auto timing = producer->pass.pollTiming();
        timing.primaryFrame = producer->primaryFrame;
        timing.primaryPublishMilliseconds = producer->publishMilliseconds;
        timing.primaryReceiveMilliseconds = producer->receiveMilliseconds;
        if (producer->mailbox)
        {
            producer->mailbox->copyObservations(timing);
        }
        return timing;
    }

    bool submitMailboxSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, uint64_t frame)
    {
        if (!producer || !producer->mailbox || !depth || (!producer->reconstructNormals && !normals) ||
            !frame || frame > UINT32_MAX || !is_multi_gpu_available())
        {
            return false;
        }
        auto& mailbox = *producer->mailbox;
        if (!mailbox.poll())
        {
            return false;
        }
        producer->pollPrimaryTiming();
        const auto image = [](BaseTexture* texture)
        {
            return drv3d_dx12::getbasetex(texture)->getDeviceImage();
        };
        auto* depthImage = image(depth);
        auto* normalImage = producer->reconstructNormals ? nullptr : image(normals);
        if (!depthImage || (!producer->reconstructNormals && !normalImage))
        {
            return false;
        }
        auto& context = drv3d_dx12::get_device().getContext();
        const auto copyInputs = [&](UINT input)
        {
            context.copySharedTexture(depthImage, mailbox.resources.inputDepthPrimary(input), true);
            if (!producer->reconstructNormals)
            {
                context.copySharedTexture(normalImage, mailbox.resources.inputNormalsPrimary(input), true);
            }
        };
        // Reserve the F0 value before choosing an output: GPU1's input wait also protects
        // reuse of retired output slots whose old GPU0 readers precede this signal.
        const uint64_t v0 = mailbox.inputsReady.reserveSignal();
        if (!v0)
        {
            return false;
        }
        const int input = mailbox.freeInput();
        const int output = mailbox.reusableOutput(v0);
        bool issue = true;
        if (input < 0)
        {
            ++mailbox.observations.skippedInput;
            issue = false;
        }
        else if (output < 0)
        {
            ++mailbox.observations.skippedOutput;
            issue = false;
        }
        else if (mailbox.runnableJobs() > 1)
        {
            ++mailbox.observations.skippedBacklog;
            issue = false;
        }

        SsaoProducer::PrimaryTiming* timing = nullptr;
        if (producer->measureTimings)
        {
            for (auto& query : producer->primaryQueries)
            {
                if (!query.pending)
                {
                    timing = &query;
                    query.frame = frame;
                    query.pending = true;
                    d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &query.begin, nullptr, nullptr);
                    break;
                }
            }
        }
        if (issue)
        {
            copyInputs(input);
            ++mailbox.observations.primaryInputCopyFrames;
        }
        if (timing)
        {
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->published, nullptr, nullptr);
        }
        // Even skipped frames submit one progress signal, allowing retired outputs/readbacks to become reusable.
        context.signalForeignFenceAfterSubmit(mailbox.inputsReady.getFence(drv3d_dx12::CrossAdapterFence::Side::SIGNALER), v0);
        ++mailbox.observations.submitOnlySignals;
        if (!issue)
        {
            return true;
        }

        drv3d_dx12::MailboxSsaoJob job;
        job.inputsReady = {mailbox.inputsReady.getFence(drv3d_dx12::CrossAdapterFence::Side::WAITER), v0};
        job.completion = &mailbox.resultsReady;
        job.frameId = mailbox.resources.outputIdSecondary(output);
        job.mailbox = mailbox.resources.mailboxSecondary();
        job.outputSlot = output;
        drv3d_dx12::MailboxSsaoCompletion done;
        const HRESULT result = producer->pass.executeMailbox(constants,
                                                             mailbox.resources.shared(input, output), job, frame, done);
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return false;
        }
        if (result == S_FALSE)
        {
            return true;
        }
        mailbox.inputs[input].readDone = done.dataReady;
        mailbox.outputs[output] = {v0, done.dataReady, done.published, frame, 0};
        return true;
    }

    bool receiveMailboxSsao(SsaoProducer* producer, BaseTexture* destination, uint64_t frame)
    {
        if (!producer || !producer->mailbox || !destination || !is_multi_gpu_available())
        {
            return false;
        }
        auto& mailbox = *producer->mailbox;
        if (!mailbox.poll())
        {
            return false;
        }
        auto* target = drv3d_dx12::getbasetex(destination)->getDeviceImage();
        if (!target)
        {
            return false;
        }
        drv3d_dx12::MailboxReceiveDesc desc;
        desc.mailbox = mailbox.resources.mailboxPrimary();
        desc.predicate = mailbox.resources.predicate();
        desc.frameId = mailbox.resources.frameId();
        for (UINT i = 0; i < drv3d_dx12::MAILBOX_OUT_SLOTS; ++i)
        {
            desc.ao[i] = mailbox.resources.outputAoPrimary(i);
            desc.id[i] = mailbox.resources.outputIdPrimary(i);
        }
        // A torn mailbox may expose old and new entries together. Copy in source-frame order,
        // keeping predicate offsets tied to their actual slots, so the newest visible ID wins.
        for (UINT i = 1; i < drv3d_dx12::MAILBOX_OUT_SLOTS; ++i)
        {
            UINT j = i;
            while (j && mailbox.outputs[desc.copyOrder[j]].frame < mailbox.outputs[desc.copyOrder[j - 1]].frame)
            {
                const UINT swap = desc.copyOrder[j];
                desc.copyOrder[j] = desc.copyOrder[j - 1];
                desc.copyOrder[j - 1] = swap;
                --j;
            }
        }
        if (producer->measureTimings)
        {
            bool sampleAllocated = false;
            for (UINT i = 0; i < drv3d_dx12::MailboxSsaoResources::READBACK_SLOTS; ++i)
            {
                if (!mailbox.samples[i].pending)
                {
                    // Next frame's F0 signal is ordered after this receive and readback on GPU0.
                    mailbox.samples[i] = {frame, mailbox.inputsReady.issuedValue() + 1, true};
                    desc.readback = mailbox.resources.readback();
                    desc.readbackOffset = i * drv3d_dx12::MailboxSsaoResources::READBACK_STRIDE;
                    sampleAllocated = true;
                    break;
                }
            }
            if (!sampleAllocated)
            {
                ++mailbox.observations.droppedAgeSamples;
            }
        }
        SsaoProducer::PrimaryTiming* timing = nullptr;
        for (auto& query : producer->primaryQueries)
        {
            if (query.pending && query.frame == frame)
            {
                timing = &query;
                break;
            }
        }
        if (timing)
        {
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->receiveBegin, nullptr, nullptr);
        }
        drv3d_dx12::get_device().getContext().mailboxReceive(target, desc);
        if (timing)
        {
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->received, nullptr, nullptr);
        }
        return true;
    }

    void destroySsaoProducer(SsaoProducer* producer)
    {
        delete producer;
        collectSsaoResources();
    }

    void collectSsaoResources()
    {
        drv3d_dx12::DeviceSsaoPass::collectRetiredResources();
        drv3d_dx12::MailboxSsaoResources::collectRetiredSetups();
    }
}  // namespace d3d::mgpu
