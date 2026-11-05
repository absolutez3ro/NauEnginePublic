// multi_gpu_ssao.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "nau/3d/multi_gpu_ssao.h"

#include "device_ssao_pass.h"
#include "framework_ssao_resources.h"
#include "device.h"
#include "texture.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau_device_resource_allocator.h"
#include "multi_gpu_device.h"
#include <cstdlib>

namespace d3d::mgpu
{
    namespace
    {
        bool usePackedSoftwareDepth(ID3D12Device* device)
        {
            // Group WARP's expensive per-texel shared-memory reads. Keep the hardware path until measured there.
            if (const char* option = std::getenv("NAU_SSAO_PACK_DEPTH"); option && option[0] == '0') return false;
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
        eastl::unique_ptr<drv3d_dx12::FrameworkSsaoResources> framework;
        bool frameworkInitialized = false;
        TransferChannelHandle normals = nullptr;
        TransferChannelHandle depth = nullptr;
        TransferChannelHandle output = nullptr;
        bool reconstructNormals = false;
        bool measureTimings = false;
        uint32_t nextResource = 0;
        struct PrimaryTiming
        {
            void* begin = nullptr;
            void* published = nullptr;
            void* received = nullptr;
            uint64_t frame = 0;
            bool pending = false;
        };
        PrimaryTiming primaryQueries[16];
        uint64_t primaryFrame = 0;
        float publishMilliseconds = -1.f;
        float receiveMilliseconds = -1.f;

        void pollPrimaryTiming()
        {
            if (!framework || !measureTimings) return;
            uint64_t frequency = 0;
            d3d::driver_command(D3V3D_COMMAND_TIMESTAMPFREQ, &frequency, nullptr, nullptr);
            for (auto& query : primaryQueries)
            {
                uint64_t begin = 0, published = 0, received = 0;
                if (!query.pending ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.begin, &begin, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.published, &published, nullptr) ||
                    !d3d::driver_command(D3V3D_COMMAND_TIMESTAMPGET, query.received, &received, nullptr)) continue;
                if (frequency && query.frame > primaryFrame && published >= begin && received >= published)
                {
                    primaryFrame = query.frame;
                    publishMilliseconds = static_cast<float>(1000.0 * static_cast<double>(published - begin) / frequency);
                    receiveMilliseconds = static_cast<float>(1000.0 * static_cast<double>(received - published) / frequency);
                }
                query.pending = false;
            }
        }

        ~SsaoProducer()
        {
            if (framework)
            {
                // Only teardown/resize drains the adapters. There is no per-frame handoff.
                drv3d_dx12::get_device().getContext().wait();
                const HRESULT result = pass.reset();
                if (FAILED(result)) NAU_LOG_WARNING("Framework SSAO: GPU1 drain failed, HRESULT {:#x}", static_cast<uint32_t>(result));
            }
            for (auto& query : primaryQueries)
            {
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.begin, nullptr, nullptr);
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.published, nullptr, nullptr);
                d3d::driver_command(DRV3D_COMMAND_RELEASE_QUERY, &query.received, nullptr, nullptr);
            }
            // Endpoints owned by the technique remain alive until all copy queues drain.
            destroy_transfer_channel(normals);
            destroy_transfer_channel(depth);
            // output owns the shared primary release fence; keep it until the input queues drain.
            destroy_transfer_channel(output);
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
            pass.inputWidth = desc.inputWidth;
            pass.inputHeight = desc.inputHeight;
            pass.vertexShader = {desc.vertexBytecode, desc.vertexBytecodeSize};
            pass.rawShader = {desc.rawBytecode, desc.rawBytecodeSize};
            pass.filterShader = {desc.filterBytecode, desc.filterBytecodeSize};
            pass.reconstructNormals = producer->reconstructNormals = desc.reconstructNormals;
            pass.packedNormals = desc.packedNormals;
            pass.linearDepth16 = desc.linearDepth16;
            pass.measureTimings = desc.measureTimings;
            pass.frameworkStyle = desc.frameworkStyle;
            pass.packedFrameworkDepth = desc.frameworkStyle && usePackedSoftwareDepth(allocator->getDevice());
            const char* normalOption = std::getenv("NAU_SSAO_PACK_NORMALS");
            pass.packedFrameworkNormals = pass.packedFrameworkDepth && !desc.reconstructNormals && (!normalOption || normalOption[0] != '0');
            for (uint32_t attempt = 0; attempt < 2; ++attempt)
            {
                result = producer->pass.init(pass);
                if (SUCCEEDED(result) && desc.frameworkStyle)
                {
                    producer->framework = eastl::make_unique<drv3d_dx12::FrameworkSsaoResources>();
                    result = producer->framework->init(static_cast<ID3D12Device*>(get_device(PRIMARY_GPU)),
                        static_cast<ID3D12Device*>(get_device(SECONDARY_GPU)), pass);
                }
                if (SUCCEEDED(result) || !pass.packedFrameworkDepth) break;
                producer->framework.reset();
                producer->pass.reset();
                pass.packedFrameworkDepth = false;
                pass.packedFrameworkNormals = false;
                NAU_LOG_WARNING("SSAO: packed software depth unavailable, HRESULT {:#x}; using ordinary shared depth", static_cast<uint32_t>(result));
            }
            if (SUCCEEDED(result) && desc.frameworkStyle)
                NAU_LOG_INFO("SSAO: packed software depth transfer={}, normals={}", pass.packedFrameworkDepth, pass.packedFrameworkNormals);
        }
        if (SUCCEEDED(result) && !desc.frameworkStyle)
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

    SsaoGpuTiming pollSsaoTiming(SsaoProducer* producer)
    {
        if (!producer) return {};
        producer->pollPrimaryTiming();
        auto timing = producer->pass.pollTiming();
        timing.primaryFrame = producer->primaryFrame;
        timing.primaryPublishMilliseconds = producer->publishMilliseconds;
        timing.primaryReceiveMilliseconds = producer->receiveMilliseconds;
        return timing;
    }

    bool exchangeFrameworkSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame, bool& submitted)
    {
        submitted = false;
        if (!producer || !producer->framework || (!producer->reconstructNormals && !normals) ||
            !depth || !destination || !is_multi_gpu_available()) return false;
        const auto image = [](BaseTexture* texture) { return drv3d_dx12::getbasetex(texture)->getDeviceImage(); };
        auto* normalImage = producer->reconstructNormals ? nullptr : image(normals);
        auto* depthImage = image(depth);
        auto* resultImage = image(destination);
        if ((!producer->reconstructNormals && !normalImage) || !depthImage || !resultImage) return false;
        const auto shared = producer->framework->primary();
        auto& context = drv3d_dx12::get_device().getContext();
        producer->pollPrimaryTiming();
        SsaoProducer::PrimaryTiming* timing = nullptr;
        if (producer->measureTimings)
            for (auto& query : producer->primaryQueries)
                if (!query.pending)
                {
                    timing = &query;
                    query.frame = frame;
                    query.pending = true;
                    d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &query.begin, nullptr, nullptr);
                    break;
                }
        context.copySharedTexture(depthImage, shared.depth, true);
        if (!producer->reconstructNormals) context.copySharedTexture(normalImage, shared.normals, true);
        if (timing) d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->published, nullptr, nullptr);
        if (!producer->frameworkInitialized)
        {
            // The caller clears destination to white on first use. Seed shared AO in the same list.
            context.copySharedTexture(resultImage, shared.result, true);
            producer->frameworkInitialized = true;
        }
        context.copySharedTexture(resultImage, shared.result, false);
        if (timing) d3d::driver_command(D3V3D_COMMAND_TIMESTAMPISSUE, &timing->received, nullptr, nullptr);
        const HRESULT result = producer->pass.executeFramework(constants, producer->framework->secondary(), frame);
        submitted = result == S_OK;
        if (FAILED(result)) report_secondary_gpu_failure(static_cast<uint32_t>(result));
        return SUCCEEDED(result);
    }

    PrimarySyncPoint submitSsao(SsaoProducer* producer, const SsaoConstants& constants, BaseTexture* normals, BaseTexture* depth, BaseTexture* destination, uint64_t frame)
    {
        if (!producer || !normals || !depth || !destination || !is_multi_gpu_available())
        {
            return {};
        }
        BaseTexture* endpoints[] = {depth, destination, normals};
        const PrimarySyncPoint released = release_primary_resources(producer->output, endpoints, producer->reconstructNormals ? 2 : 3);
        if (!released.nativeFence)
        {
            report_secondary_gpu_failure(static_cast<uint32_t>(E_FAIL));
            return {};
        }
        const uint32_t resourceIndex = producer->nextResource;
        const PrimarySend normalSent = producer->reconstructNormals ? PrimarySend{} : send_from_primary_deferred(producer->normals, normals, released);
        const PrimarySend depthSent = send_from_primary_deferred(producer->depth, depth, released);
        const drv3d_dx12::CrossAdapterSyncPoint free = producer->pass.getReadyPoint(resourceIndex);
        const SecondarySyncPoint destinationFree = {free.fence, free.value};
        // Consume partial sends as well, so failure recovery can drain every issued copy.
        const SecondarySyncPoint normalReady = normalSent.ticket
                                                   ? receive_on_secondary(producer->normals, normalSent.ticket, producer->pass.getNormals(resourceIndex), destinationFree)
                                                   : SecondarySyncPoint{};
        const SecondarySyncPoint depthReady = depthSent.ticket
                                                  ? receive_on_secondary(producer->depth, depthSent.ticket, producer->pass.getDepth(resourceIndex), destinationFree)
                                                  : SecondarySyncPoint{};
        HRESULT result = (producer->reconstructNormals || normalReady.nativeFence) && depthReady.nativeFence ? S_OK : E_FAIL;
        if (SUCCEEDED(result))
        {
            result = producer->pass.execute(constants,
                                            {static_cast<ID3D12Fence*>(normalReady.nativeFence), normalReady.value},
                                            {static_cast<ID3D12Fence*>(depthReady.nativeFence), depthReady.value}, frame, resourceIndex);
        }
        if (SUCCEEDED(result))
        {
            const drv3d_dx12::CrossAdapterSyncPoint rendered = producer->pass.getReadyPoint(resourceIndex);
            const SecondarySend sent = send_from_secondary(producer->output, producer->pass.getResult(resourceIndex), {rendered.fence, rendered.value});
            result = sent.ticket ? producer->pass.releaseOutput({static_cast<ID3D12Fence*>(sent.sourceReleased.nativeFence), sent.sourceReleased.value}, resourceIndex) : E_FAIL;
            if (SUCCEEDED(result))
            {
                // The copy queue waits for AO on GPU 1. Graphics waits only later, at ssao_receive.
                const PrimarySyncPoint receiving = begin_receive_on_primary(producer->output, sent.ticket, destination, released);
                if (receiving.nativeFence)
                {
                    producer->nextResource = (resourceIndex + 1) % drv3d_dx12::DeviceSsaoPass::RESOURCE_COUNT;
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
