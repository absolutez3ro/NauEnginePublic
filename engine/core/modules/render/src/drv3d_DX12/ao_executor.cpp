// ao_executor.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/ao_executor.h"

#include "ao_device_pass.h"
#include "device.h"
#include "mgpu_context.h"
#include "multi_gpu_device.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau/mgpu/mailbox/mailbox_state.h"
#include "texture.h"

#if defined(NAU_ENABLE_MGPU)
namespace nau::render::ao
{
    using d3d::is_multi_gpu_available;
    using d3d::PRIMARY_GPU;
    using d3d::report_secondary_gpu_failure;
    using d3d::SECONDARY_GPU;
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
    }  // namespace

    struct SecondaryExecutor
    {
        SecondaryExecutor() = default;
        SecondaryExecutor(const SecondaryExecutor&) = delete;
        SecondaryExecutor(SecondaryExecutor&&) = delete;
        SecondaryExecutor& operator=(const SecondaryExecutor&) = delete;
        SecondaryExecutor& operator=(SecondaryExecutor&&) = delete;

        drv3d_dx12::AoDevicePass pass;
        eastl::shared_ptr<nau::mgpu::MailboxState> mailbox;
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

        ~SecondaryExecutor()
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

    SecondaryExecutor* createSecondaryExecutor(const ExecutorDescription& desc)
    {
        if (!is_multi_gpu_available())
        {
            return nullptr;
        }
        const auto primary = drv3d_dx12::get_mgpu_device_context(PRIMARY_GPU);
        const auto secondary = drv3d_dx12::get_mgpu_device_context(SECONDARY_GPU);
        HRESULT result = primary && secondary ? S_OK : E_FAIL;
        eastl::unique_ptr<SecondaryExecutor> producer = eastl::make_unique<SecondaryExecutor>();
        producer->measureTimings = desc.measureTimings;
        if (SUCCEEDED(result))
        {
            drv3d_dx12::AoDevicePassDesc pass;
            pass.context = secondary;
            pass.width = desc.width;
            pass.height = desc.height;
            pass.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            pass.rawShader = {desc.rawBytecode, desc.rawBytecodeSize};
            pass.rawComputeShader = {desc.rawComputeBytecode, desc.rawComputeBytecodeSize};
            pass.createAlgorithm = desc.createAlgorithm;
            pass.algorithmShaders = desc.algorithmShaders;
            pass.algorithmShaderCount = desc.algorithmShaderCount;
            pass.reconstructNormals = producer->reconstructNormals = desc.reconstructNormals;
            pass.measureTimings = desc.measureTimings;
            pass.packedFrameworkDepth = usePackedSoftwareDepth(secondary->device());
            pass.packedFrameworkNormals = pass.packedFrameworkDepth && !desc.reconstructNormals;
            for (uint32_t attempt = 0; attempt < 2; ++attempt)
            {
                result = producer->pass.init(pass);
                if (SUCCEEDED(result))
                {
                    producer->mailbox = eastl::make_shared<nau::mgpu::MailboxState>();
                    nau::mgpu::MailboxDescription layout;
                    layout.inputCount = desc.reconstructNormals ? 1 : 2;
                    layout.inputs[0] = {desc.width, desc.height, DXGI_FORMAT_R32_FLOAT,
                                        pass.packedFrameworkDepth ? (desc.width + 3) / 4 : 0, DXGI_FORMAT_R32G32B32A32_UINT};
                    layout.inputs[1] = {desc.width, desc.height, DXGI_FORMAT_R16G16_FLOAT,
                                        pass.packedFrameworkNormals ? (desc.width + 3) / 4 : 0, DXGI_FORMAT_R32G32B32A32_UINT};
                    layout.output = {desc.width, desc.height, DXGI_FORMAT_R8_UNORM};
                    result = producer->mailbox->init(primary, secondary, layout);
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

    ExecutionStats pollSecondary(SecondaryExecutor* producer)
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
            const auto& observation = producer->mailbox->observations;
            timing.mailbox = timing.gpuMailbox = true;
            timing.displayedFrame = observation.displayedFrame;
            timing.displayedAoFrame = observation.displayedSourceFrame;
            timing.ageSamples = observation.ageSamples;
            std::memcpy(timing.ageHistogram, observation.ageHistogram, sizeof(timing.ageHistogram));
            timing.skippedInput = observation.skippedInput;
            timing.skippedOutput = observation.skippedOutput;
            timing.skippedBacklog = observation.skippedBacklog;
            timing.submitOnlySignals = observation.submitOnlySignals;
            timing.badFrameIds = observation.badFrameIds;
            timing.unavailableAoFrames = observation.unavailableFrames;
            timing.droppedAgeSamples = observation.droppedAgeSamples;
            timing.primaryInputCopyFrames = observation.inputCopyFrames;
        }
        return timing;
    }

    bool submitSecondary(SecondaryExecutor* producer, const nau::render::AoConstants& constants, BaseTexture* normals, BaseTexture* depth, uint64_t frame)
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
            context.copySharedTexture(depthImage, mailbox.resources.input(input, 0, 0), true);
            if (!producer->reconstructNormals)
            {
                context.copySharedTexture(normalImage, mailbox.resources.input(input, 1, 0), true);
            }
        };
        const auto reservation = mailbox.reserve();
        const auto v0 = reservation.inputValue;
        const auto input = reservation.input;
        const auto output = reservation.output;
        const bool issue = reservation.accepted;
        if (!v0)
        {
            return false;
        }

        SecondaryExecutor::PrimaryTiming* timing = nullptr;
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
            ++mailbox.observations.inputCopyFrames;
        }
        if (timing)
        {
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->published, nullptr, nullptr);
        }
        // Even skipped frames submit one progress signal, allowing retired outputs/readbacks to become reusable.
        context.signalForeignFenceAfterSubmit(mailbox.inputsReady.getFence(nau::mgpu::CrossAdapterFence::Side::SIGNALER), v0);
        ++mailbox.observations.submitOnlySignals;
        if (!issue)
        {
            return true;
        }

        nau::mgpu::MailboxJob job;
        job.inputsReady = {mailbox.inputsReady.getFence(nau::mgpu::CrossAdapterFence::Side::WAITER), v0};
        job.completion = &mailbox.resultsReady;
        job.frameId = mailbox.resources.outputId(output, 1);
        job.mailbox = mailbox.resources.mailboxSecondary();
        job.outputSlot = output;
        nau::mgpu::MailboxCompletion done;
        const HRESULT result = producer->pass.executeMailbox(constants,
                                                             {mailbox.resources.input(input, 1, 1), mailbox.resources.input(input, 0, 1), mailbox.resources.output(output, 1)}, job, frame, done);
        if (FAILED(result))
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(result));
            return false;
        }
        if (result == S_FALSE)
        {
            return true;
        }
        mailbox.commit(reservation, done.dataReady, done.published, frame);
        return true;
    }

    bool receiveSecondary(SecondaryExecutor* producer, BaseTexture* destination, uint64_t frame)
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
        const auto desc = mailbox.receive(frame, producer->measureTimings);
        SecondaryExecutor::PrimaryTiming* timing = nullptr;
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

    void destroySecondaryExecutor(SecondaryExecutor* producer)
    {
        delete producer;
        collectSecondaryResources();
    }

    void collectSecondaryResources()
    {
        drv3d_dx12::AoDevicePass::collectRetiredResources();
        nau::mgpu::MailboxResources::collectRetiredSetups();
    }
}  // namespace nau::render::ao

#else
namespace nau::render::ao
{
    SecondaryExecutor* createSecondaryExecutor(const ExecutorDescription&)
    {
        return nullptr;
    }
    ExecutionStats pollSecondary(SecondaryExecutor*)
    {
        return {};
    }
    bool submitSecondary(SecondaryExecutor*, const nau::render::AoConstants&, BaseTexture*, BaseTexture*, uint64_t)
    {
        return false;
    }
    bool receiveSecondary(SecondaryExecutor*, BaseTexture*, uint64_t)
    {
        return false;
    }
    void destroySecondaryExecutor(SecondaryExecutor*)
    {
    }
    void collectSecondaryResources()
    {
    }
}  // namespace nau::render::ao
#endif
