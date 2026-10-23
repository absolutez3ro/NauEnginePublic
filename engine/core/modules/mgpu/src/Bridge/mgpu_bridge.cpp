// Mgpu <-> Nau transfer bridge, see include/nau/mgpu/bridge/mgpu_bridge.h.
// No Nau headers here: the Mgpu headers below leak global `using namespace` directives and macros.

#include "nau/mgpu/bridge/mgpu_bridge.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Utils/d3dx12.h"
#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Graphics/GCommandQueue.h"
#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GDeviceFactory.h"
#include "nau/mgpu/Graphics/GResource.h"
#include "nau/mgpu/Graphics/GTexture.h"
#include "nau/mgpu/Common/GCrossAdapterResource.h"

namespace nau::mgpu::bridge
{
    namespace gfx = nau::PEPEngine::Graphics;

    namespace
    {
        std::atomic<bool> g_enabled{false};

        constexpr uint32_t kWidth = 256;
        constexpr uint32_t kHeight = 256;
        constexpr uint32_t kBytesPerPixel = 4;
        constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        std::string ToUtf8(const std::wstring& wide)
        {
            if (wide.empty())
            {
                return {};
            }

            const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                                 nullptr, nullptr);
            std::string result(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), size, nullptr,
                                nullptr);
            return result;
        }

        std::wstring ToWide(const char* utf8)
        {
            if (utf8 == nullptr || utf8[0] == '\0')
            {
                return {};
            }

            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
            if (size <= 1)
            {
                return {};
            }

            std::wstring result(static_cast<size_t>(size - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), size);
            return result;
        }

        // The test pattern: R scrolls with the frame, G is the row, B is a 16x16 checker, A is opaque.
        void FillPatternRow(uint8_t* row, const uint32_t y, const uint32_t phase)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                uint8_t* pixel = row + x * kBytesPerPixel;
                pixel[0] = static_cast<uint8_t>((x + phase) & 0xFF);
                pixel[1] = static_cast<uint8_t>(y & 0xFF);
                pixel[2] = (((x >> 4) + (y >> 4)) & 1) != 0 ? 255 : 0;
                pixel[3] = 255;
            }
        }

        uint64_t Fnv1a(const uint8_t* data, const size_t size)
        {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }
    }

    void setEnabled(const bool enabled)
    {
        g_enabled.store(enabled);
    }

    bool isEnabled()
    {
        return g_enabled.load();
    }

    struct TransferBridge::Impl
    {
        bool ready = false;
        std::string error;
        std::string primaryName;
        std::string secondaryName;

        std::shared_ptr<gfx::GDevice> primaryDevice;
        std::shared_ptr<gfx::GDevice> secondaryDevice;

        // GPU2 side: CPU upload buffer -> source texture -> shared heap.
        std::shared_ptr<gfx::GResource> uploadBuffer;
        std::shared_ptr<gfx::GTexture> sourceTexture;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT uploadFootprint = {};

        // Shared heap with two placed resources: ::GCrossAdapterResource (Common variant).
        std::shared_ptr<GCrossAdapterResource> shared;

        // GPU1 side: shared heap -> read-back buffer.
        std::shared_ptr<gfx::GResource> readbackBuffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint = {};

        // GPU2 -> GPU1 ordering on the GPU: Signal on the second device, Wait on the primary one.
        ComPtr<ID3D12Fence> primaryFence;
        ComPtr<ID3D12Fence> secondaryFence;
        UINT64 fenceValue = 0;

        std::vector<uint8_t> pixels;
        TransferInfo info;

        bool Initialize(const char* nauAdapterName)
        {
            try
            {
                // Inside a process that already owns a D3D12 device (Nau) the debug layer must not be enabled
                // after the fact. A standalone caller passes no adapter name and keeps the default.
                if (nauAdapterName != nullptr)
                {
                    gfx::GDeviceFactory::SetDebugLayerEnabled(false);
                }

                auto& devices = gfx::GDeviceFactory::GetAllDevices(false);
                if (devices.size() < 2)
                {
                    error = "need two distinct hardware adapters, found " + std::to_string(devices.size());
                    return false;
                }

                size_t primaryIndex = 0;
                const std::wstring wantedName = ToWide(nauAdapterName);
                bool matched = wantedName.empty();
                for (size_t i = 0; i < devices.size() && !matched; ++i)
                {
                    const std::wstring name = devices[i]->GetName();
                    if (name == wantedName || name.find(wantedName) != std::wstring::npos ||
                        wantedName.find(name) != std::wstring::npos)
                    {
                        primaryIndex = i;
                        matched = true;
                    }
                }

                if (!matched)
                {
                    error = "Nau adapter '" + ToUtf8(wantedName) + "' was not found among the Mgpu adapters";
                    return false;
                }

                primaryDevice = devices[primaryIndex];
                secondaryDevice = devices[primaryIndex == 0 ? 1 : 0];
                primaryName = ToUtf8(primaryDevice->GetName());
                secondaryName = ToUtf8(secondaryDevice->GetName());

                // Source texture on GPU2 and the upload buffer that feeds it.
                const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(kFormat, kWidth, kHeight, 1, 1);
                sourceTexture = std::make_shared<gfx::GTexture>(secondaryDevice, textureDesc, L"MgpuBridge Source");

                UINT64 uploadSize = 0;
                secondaryDevice->GetDXDevice()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &uploadFootprint, nullptr,
                                                                      nullptr, &uploadSize);
                uploadBuffer = std::make_shared<gfx::GResource>(
                    secondaryDevice, CD3DX12_RESOURCE_DESC::Buffer(uploadSize), L"MgpuBridge Upload", nullptr,
                    D3D12_RESOURCE_STATE_GENERIC_READ, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD));

                // Shared heap: placed resource on GPU1 (prime) and on GPU2 (shared).
                D3D12_RESOURCE_DESC sharedDesc = textureDesc;
                shared = std::make_shared<GCrossAdapterResource>(sharedDesc, primaryDevice, secondaryDevice,
                                                                 L"MgpuBridge Shared");

                // Read-back buffer on GPU1 sized for the row-major (256-byte aligned pitch) shared texture.
                UINT64 readbackSize = 0;
                primaryDevice->GetDXDevice()->GetCopyableFootprints(&sharedDesc, 0, 1, 0, &readbackFootprint, nullptr,
                                                                    nullptr, &readbackSize);
                readbackBuffer = std::make_shared<gfx::GResource>(
                    primaryDevice, CD3DX12_RESOURCE_DESC::Buffer(readbackSize), L"MgpuBridge Readback", nullptr,
                    D3D12_RESOURCE_STATE_COPY_DEST, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK));

                // GPU-side ordering between the two devices. If a shared fence cannot be created the update falls
                // back to waiting for GPU2 on the CPU.
                primaryDevice->TrySharedFence(primaryFence, secondaryDevice, secondaryFence, 0);

                pixels.assign(static_cast<size_t>(kWidth) * kHeight * kBytesPerPixel, 0);
                info = {};
                info.width = kWidth;
                info.height = kHeight;
                info.rowPitch = kWidth * kBytesPerPixel;

                ready = true;
                error.clear();
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during initialize: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during initialize (see the D3D12 debug output)";
            }

            Release();
            return false;
        }

        bool Update(const uint32_t frameIndex)
        {
            if (!ready)
            {
                return false;
            }

            try
            {
                const uint32_t phase = frameIndex & 0xFF;

                // 1. CPU pattern -> upload buffer (GPU2).
                {
                    uint8_t* mapped = nullptr;
                    const D3D12_RANGE noRead = {0, 0};
                    ThrowIfFailed(uploadBuffer->GetD3D12Resource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
                    for (uint32_t y = 0; y < kHeight; ++y)
                    {
                        FillPatternRow(mapped + uploadFootprint.Offset + static_cast<size_t>(y) *
                                       uploadFootprint.Footprint.RowPitch, y, phase);
                    }
                    uploadBuffer->GetD3D12Resource()->Unmap(0, nullptr);
                }

                // 2. GPU2: upload buffer -> source texture -> shared heap.
                auto secondaryQueue = secondaryDevice->GetCommandQueue(gfx::GQueueType::Graphics);
                {
                    auto cmdList = secondaryQueue->GetCommandList();

                    cmdList->TransitionBarrier(*sourceTexture, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->FlushResourceBarriers();

                    const CD3DX12_TEXTURE_COPY_LOCATION dst(sourceTexture->GetD3D12Resource().Get(), 0);
                    const CD3DX12_TEXTURE_COPY_LOCATION src(uploadBuffer->GetD3D12Resource().Get(), uploadFootprint);
                    cmdList->GetGraphicsCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                    cmdList->CopyResource(shared->GetSharedResource(), *sourceTexture);
                    cmdList->TransitionBarrier(shared->GetSharedResource(), D3D12_RESOURCE_STATE_COMMON);
                    cmdList->TransitionBarrier(*sourceTexture, D3D12_RESOURCE_STATE_COMMON);
                    cmdList->FlushResourceBarriers();

                    secondaryQueue->ExecuteCommandList(cmdList);
                }

                // 3. Order GPU1 after GPU2.
                auto primaryQueue = primaryDevice->GetCommandQueue(gfx::GQueueType::Copy);
                if (primaryFence && secondaryFence)
                {
                    ++fenceValue;
                    secondaryQueue->Signal(secondaryFence, fenceValue);
                    primaryQueue->Wait(primaryFence, fenceValue);
                }
                else
                {
                    secondaryQueue->WaitForFenceValue(secondaryQueue->Signal());
                }

                // 4. GPU1: shared heap -> read-back buffer.
                uint64_t primaryFenceValue = 0;
                {
                    auto cmdList = primaryQueue->GetCommandList();

                    cmdList->TransitionBarrier(shared->GetPrimeResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    cmdList->FlushResourceBarriers();

                    const CD3DX12_TEXTURE_COPY_LOCATION dst(readbackBuffer->GetD3D12Resource().Get(),
                                                            readbackFootprint);
                    const CD3DX12_TEXTURE_COPY_LOCATION src(shared->GetPrimeResource().GetD3D12Resource().Get(), 0);
                    cmdList->GetGraphicsCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                    cmdList->TransitionBarrier(shared->GetPrimeResource(), D3D12_RESOURCE_STATE_COMMON);
                    cmdList->FlushResourceBarriers();

                    primaryFenceValue = primaryQueue->ExecuteCommandList(cmdList);
                }
                primaryQueue->WaitForFenceValue(primaryFenceValue);

                // 5. Read-back buffer -> tightly packed CPU pixels.
                {
                    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(readbackFootprint.Footprint.RowPitch) *
                                                   kHeight + static_cast<SIZE_T>(readbackFootprint.Offset)};
                    const uint8_t* mapped = nullptr;
                    ThrowIfFailed(readbackBuffer->GetD3D12Resource()->Map(0, &readRange,
                                                                          reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
                    for (uint32_t y = 0; y < kHeight; ++y)
                    {
                        std::memcpy(pixels.data() + static_cast<size_t>(y) * kWidth * kBytesPerPixel,
                                    mapped + readbackFootprint.Offset +
                                    static_cast<size_t>(y) * readbackFootprint.Footprint.RowPitch,
                                    static_cast<size_t>(kWidth) * kBytesPerPixel);
                    }
                    const D3D12_RANGE noWrite = {0, 0};
                    readbackBuffer->GetD3D12Resource()->Unmap(0, &noWrite);
                }

                // 6. Verify against the pattern generated on the CPU.
                std::vector<uint8_t> expected(pixels.size());
                for (uint32_t y = 0; y < kHeight; ++y)
                {
                    FillPatternRow(expected.data() + static_cast<size_t>(y) * kWidth * kBytesPerPixel, y, phase);
                }

                info.checksum = Fnv1a(pixels.data(), pixels.size());
                info.expectedChecksum = Fnv1a(expected.data(), expected.size());
                info.checksumMatches = info.checksum == info.expectedChecksum;

                error = info.checksumMatches ? std::string() : std::string("received pixels differ from the expected pattern");
                return info.checksumMatches;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during update: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during update (see the D3D12 debug output)";
            }

            info.checksumMatches = false;
            return false;
        }

        void Release()
        {
            ready = false;

            if (primaryDevice)
            {
                primaryDevice->Flush();
            }
            if (secondaryDevice)
            {
                secondaryDevice->Flush();
            }

            primaryFence.Reset();
            secondaryFence.Reset();
            readbackBuffer.reset();
            shared.reset();
            uploadBuffer.reset();
            sourceTexture.reset();
            primaryDevice.reset();
            secondaryDevice.reset();
        }
    };

    TransferBridge::TransferBridge() : m_impl(std::make_unique<Impl>())
    {
    }

    TransferBridge::~TransferBridge()
    {
        shutdown();
    }

    bool TransferBridge::initialize(const char* nauAdapterName)
    {
        if (m_impl->ready)
        {
            return true;
        }
        return m_impl->Initialize(nauAdapterName);
    }

    bool TransferBridge::update(const uint32_t frameIndex)
    {
        return m_impl->Update(frameIndex);
    }

    bool TransferBridge::isReady() const
    {
        return m_impl->ready;
    }

    void TransferBridge::shutdown()
    {
        if (m_impl)
        {
            m_impl->Release();
        }
    }

    const char* TransferBridge::lastError() const
    {
        return m_impl->error.c_str();
    }

    const char* TransferBridge::primaryAdapterName() const
    {
        return m_impl->primaryName.c_str();
    }

    const char* TransferBridge::secondaryAdapterName() const
    {
        return m_impl->secondaryName.c_str();
    }

    const void* TransferBridge::pixels() const
    {
        return m_impl->pixels.empty() ? nullptr : m_impl->pixels.data();
    }

    const TransferInfo& TransferBridge::info() const
    {
        return m_impl->info;
    }
}
