// Second-GPU renderer of the UI layer, see include/nau/mgpu/bridge/mgpu_ui_layer.h.
// The transfer (slots, shared heap, copy queue of GPU0) repeats the one of the shadow atlas (mgpu_shadows.cpp).
// No Nau headers here: the Mgpu headers below leak global `using namespace` directives and macros.

#include "nau/mgpu/bridge/mgpu_ui_layer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <string>

#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Utils/d3dx12.h"
#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Graphics/GCommandQueue.h"
#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GResource.h"
#include "nau/mgpu/Common/GCrossAdapterResource.h"

#include "bridge_common.h"

namespace nau::mgpu::ui_layer
{
    namespace gfx = nau::PEPEngine::Graphics;

    namespace
    {
        constexpr uint32_t kSlotCount = SLOT_COUNT;

        // The copy into Nau's texture (DRV3D_COMMAND_D3D12_COPY_FROM_EXTERNAL_RESOURCE) needs exactly the format
        // drv3d_DX12 created that texture with: TEXFMT_R8G8B8A8 | TEXCF_RTARGET without sRGB flags is
        // R8G8B8A8_UNORM (checked: the driver refused R8G8B8A8_TYPELESS for it). UNORM: the UI is written without sRGB
        // conversion, as into Nau's ui_layer.
        constexpr DXGI_FORMAT kLayerFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT kLayerViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    struct LayerRenderer::Impl
    {
        using Clock = std::chrono::steady_clock;

        enum class SlotState
        {
            Free,
            Rendering, // submitted to GPU1
            Drawn, // finished on GPU1, in the shared heap
            Transferring, // shared heap -> local memory of GPU0, on its copy queue
            Ready, // in local memory of GPU0, not handed out yet
            Handed // given to Nau, waits for release()
        };

        struct Slot
        {
            SlotState state = SlotState::Free;
            std::shared_ptr<GCrossAdapterResource> shared; // shared heap, placed resource on each GPU
            std::shared_ptr<gfx::GResource> local; // copy in local memory of GPU0, handed to Nau
            uint32_t width = 0;
            uint32_t height = 0;
            uint64_t fenceValue = 0; // graphics queue of GPU1
            uint64_t transferFenceValue = 0; // copy queue of GPU0
            uint64_t sequence = 0;
            uint64_t userToken = 0;
            Clock::time_point submitted;
            Clock::time_point drawn;
            float latencyMs = 0;
        };

        bool ready = false;
        std::string error;
        std::string primaryName;
        std::string secondaryName;

        std::shared_ptr<gfx::GDevice> primaryDevice; // GPU0, Nau's adapter
        std::shared_ptr<gfx::GDevice> secondaryDevice; // GPU1

        // The layer on GPU1. One is enough: the work of all slots is ordered on one queue.
        std::shared_ptr<gfx::GResource> layer;
        gfx::GDescriptor layerRtv;
        uint32_t layerWidth = 0;
        uint32_t layerHeight = 0;

        std::array<Slot, kSlotCount> slots;
        uint64_t nextSequence = 0;
        RendererStats stats;

        std::shared_ptr<gfx::GCommandQueue>& GraphicsQueue1() const
        {
            return secondaryDevice->GetCommandQueue(gfx::GQueueType::Graphics);
        }

        std::shared_ptr<gfx::GCommandQueue>& CopyQueue0() const
        {
            return primaryDevice->GetCommandQueue(gfx::GQueueType::Copy);
        }

        bool Initialize(const char* nauAdapterName)
        {
            try
            {
                if (!bridge::detail::SelectDevices(nauAdapterName, primaryDevice, secondaryDevice, error))
                {
                    return false;
                }
                primaryName = bridge::detail::ToUtf8(primaryDevice->GetName());
                secondaryName = bridge::detail::ToUtf8(secondaryDevice->GetName());

                stats = {};
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

        // The render target on GPU1; recreated when the window size changes (waits for GPU1: the previous layer may
        // still be in use).
        void EnsureLayer(const uint32_t width, const uint32_t height)
        {
            if (layer && layerWidth == width && layerHeight == height)
            {
                return;
            }

            if (layer)
            {
                GraphicsQueue1()->Flush();
            }

            const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(kLayerFormat, width, height, 1, 1, 1, 0,
                                                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = kLayerViewFormat; // transparent black, the colour the layer is cleared to every frame
            layer = std::make_shared<gfx::GResource>(secondaryDevice, desc, L"MgpuUiLayer Layer", &clear);

            if (layerRtv.IsNull())
            {
                layerRtv = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
            }
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = kLayerViewFormat;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            layer->CreateRenderTargetView(&rtvDesc, &layerRtv);

            layerWidth = width;
            layerHeight = height;
        }

        void EnsureSlot(Slot& slot, const uint32_t width, const uint32_t height)
        {
            if (slot.shared && slot.width == width && slot.height == height)
            {
                return;
            }

            // GCrossAdapterResource replaces the flags with ALLOW_CROSS_ADAPTER and the layout with ROW_MAJOR.
            D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(kLayerFormat, width, height, 1, 1);
            const auto index = static_cast<size_t>(&slot - slots.data());
            slot.shared = std::make_shared<GCrossAdapterResource>(desc, primaryDevice, secondaryDevice,
                                                                  L"MgpuUiLayer Shared " + std::to_wstring(index));
            // Nau copies from local memory: reading the shared heap (system memory) is left to the copy queue of GPU0,
            // so it does not take time from Nau's graphics queue. A new description: GCrossAdapterResource has changed
            // `desc`.
            const D3D12_RESOURCE_DESC localDesc = CD3DX12_RESOURCE_DESC::Tex2D(kLayerFormat, width, height, 1, 1);
            slot.local = std::make_shared<gfx::GResource>(primaryDevice, localDesc, L"MgpuUiLayer Local " + std::to_wstring(index));
            slot.width = width;
            slot.height = height;
        }

        Slot* FreeSlot()
        {
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Free)
                {
                    return &slot;
                }
            }
            return nullptr;
        }

        bool Submit(const FrameDesc& frame)
        {
            if (!ready || frame.width == 0 || frame.height == 0)
            {
                return false;
            }

            // GPU1 starts a new layer only when it has finished the previous one (as for the shadows).
            CollectFinished();
            const bool rendering = std::any_of(slots.begin(), slots.end(), [](const Slot& item)
            {
                return item.state == SlotState::Rendering;
            });
            Slot* slot = rendering ? nullptr : FreeSlot();
            if (slot == nullptr)
            {
                ++stats.skippedBusy;
                return false;
            }

            try
            {
                EnsureLayer(frame.width, frame.height);
                EnsureSlot(*slot, frame.width, frame.height);

                auto queue = GraphicsQueue1();
                auto cmdList = queue->GetCommandList();

                // Stage A content: transparent layer with one test rectangle.
                cmdList->TransitionBarrier(*layer, D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmdList->FlushResourceBarriers();
                const FLOAT transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                cmdList->ClearRenderTarget(&layerRtv, 0, transparent);

                const uint32_t x = std::min(frame.fillRect[0], frame.width);
                const uint32_t y = std::min(frame.fillRect[1], frame.height);
                const uint32_t right = std::min(frame.fillRect[0] + frame.fillRect[2], frame.width);
                const uint32_t bottom = std::min(frame.fillRect[1] + frame.fillRect[3], frame.height);
                if (right > x && bottom > y)
                {
                    const D3D12_RECT rect = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(right),
                                             static_cast<LONG>(bottom)};
                    cmdList->ClearRenderTarget(&layerRtv, 0, frame.fillColor, &rect, 1);
                }

                // Layer -> shared heap, then both back to COMMON (as the shadow atlas).
                cmdList->CopyResource(slot->shared->GetSharedResource(), *layer);
                cmdList->TransitionBarrier(slot->shared->GetSharedResource(), D3D12_RESOURCE_STATE_COMMON);
                cmdList->TransitionBarrier(*layer, D3D12_RESOURCE_STATE_COMMON);
                cmdList->FlushResourceBarriers();

                slot->fenceValue = queue->ExecuteCommandList(cmdList);
                slot->state = SlotState::Rendering;
                slot->sequence = ++nextSequence;
                slot->userToken = frame.userToken;
                slot->submitted = Clock::now();
                ++stats.submitted;

                if (frame.waitForResult)
                {
                    // Synchronous mode: GPU1 draws, then the copy queue of GPU0 moves the layer, both waited for here.
                    queue->WaitForFenceValue(slot->fenceValue);
                    slot->state = SlotState::Drawn;
                    StartTransfer(*slot);
                    if (slot->state == SlotState::Transferring)
                    {
                        CopyQueue0()->WaitForFenceValue(slot->transferFenceValue);
                        CollectFinished();
                    }
                }
                return true;
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during submit: ") + e.what();
            }
            catch (...)
            {
                error = "unknown exception during submit (see the D3D12 debug output)";
            }

            ++stats.failed;
            return false;
        }

        // GPU0 copies the newest layer of GPU1 when its copy queue is free.
        void StartTransfer(Slot& slot)
        {
            try
            {
                auto queue = CopyQueue0();
                auto cmdList = queue->GetCommandList();
                const gfx::GResource& prime = slot.shared->GetPrimeResource();
                cmdList->CopyResource(*slot.local, prime);
                cmdList->TransitionBarrier(*slot.local, D3D12_RESOURCE_STATE_COMMON);
                cmdList->TransitionBarrier(prime, D3D12_RESOURCE_STATE_COMMON);
                cmdList->FlushResourceBarriers();
                slot.transferFenceValue = queue->ExecuteCommandList(cmdList);
                slot.state = SlotState::Transferring;
                slot.drawn = Clock::now();
            }
            catch (const std::exception& e)
            {
                error = std::string("exception during the transfer to GPU0: ") + e.what();
                slot.state = SlotState::Free;
                ++stats.failed;
            }
        }

        void CollectFinished()
        {
            auto graphics1 = GraphicsQueue1();
            auto copy0 = CopyQueue0();
            bool transferring = false;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Rendering && graphics1->IsFinish(slot.fenceValue))
                {
                    slot.state = SlotState::Drawn;
                }
                else if (slot.state == SlotState::Transferring)
                {
                    if (copy0->IsFinish(slot.transferFenceValue))
                    {
                        const Clock::time_point now = Clock::now();
                        slot.state = SlotState::Ready;
                        slot.latencyMs = std::chrono::duration<float, std::milli>(now - slot.submitted).count();
                        stats.lastLatencyMs = slot.latencyMs;
                        stats.lastTransferMs = std::chrono::duration<float, std::milli>(now - slot.drawn).count();
                        ++stats.completed;
                    }
                    else
                    {
                        transferring = true;
                    }
                }
            }

            if (transferring)
            {
                return;
            }

            // Only the newest drawn layer is worth the transfer; older ones are superseded.
            Slot* newest = nullptr;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Drawn && (newest == nullptr || slot.sequence > newest->sequence))
                {
                    newest = &slot;
                }
            }
            if (newest == nullptr)
            {
                return;
            }
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Drawn && &slot != newest)
                {
                    slot.state = SlotState::Free;
                }
            }
            StartTransfer(*newest);
        }

        bool TakeReady(ReadyLayer& out)
        {
            if (!ready)
            {
                return false;
            }

            CollectFinished();

            Slot* newest = nullptr;
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Ready && (newest == nullptr || slot.sequence > newest->sequence))
                {
                    newest = &slot;
                }
            }
            if (newest == nullptr)
            {
                return false;
            }

            // Older finished layers are superseded by the newest one.
            for (Slot& slot : slots)
            {
                if (slot.state == SlotState::Ready && &slot != newest)
                {
                    slot.state = SlotState::Free;
                }
            }

            newest->state = SlotState::Handed;
            out.slot = static_cast<uint32_t>(newest - slots.data());
            out.resource = newest->local->GetD3D12Resource().Get();
            out.width = newest->width;
            out.height = newest->height;
            out.userToken = newest->userToken;
            out.latencyMs = newest->latencyMs;
            return true;
        }

        void ReleaseSlot(const uint32_t index)
        {
            if (index < kSlotCount && slots[index].state == SlotState::Handed)
            {
                slots[index].state = SlotState::Free;
            }
        }

        void Release()
        {
            ready = false;

            // Waits for all work in flight on both GPUs.
            if (primaryDevice)
            {
                primaryDevice->Flush();
            }
            if (secondaryDevice)
            {
                secondaryDevice->Flush();
            }

            for (Slot& slot : slots)
            {
                slot = Slot();
            }
            layerRtv = gfx::GDescriptor();
            layer.reset();
            layerWidth = 0;
            layerHeight = 0;
            primaryDevice.reset();
            secondaryDevice.reset();
        }
    };

    LayerRenderer::LayerRenderer() : m_impl(std::make_unique<Impl>())
    {
    }

    LayerRenderer::~LayerRenderer()
    {
        shutdown();
    }

    bool LayerRenderer::initialize(const char* nauAdapterName)
    {
        if (m_impl->ready)
        {
            return true;
        }
        return m_impl->Initialize(nauAdapterName);
    }

    bool LayerRenderer::isReady() const
    {
        return m_impl->ready;
    }

    void LayerRenderer::shutdown()
    {
        if (m_impl)
        {
            m_impl->Release();
        }
    }

    bool LayerRenderer::submit(const FrameDesc& frame)
    {
        return m_impl->Submit(frame);
    }

    bool LayerRenderer::takeReady(ReadyLayer& out)
    {
        return m_impl->TakeReady(out);
    }

    void LayerRenderer::release(const uint32_t slot)
    {
        m_impl->ReleaseSlot(slot);
    }

    const char* LayerRenderer::lastError() const
    {
        return m_impl->error.c_str();
    }

    const char* LayerRenderer::primaryAdapterName() const
    {
        return m_impl->primaryName.c_str();
    }

    const char* LayerRenderer::secondaryAdapterName() const
    {
        return m_impl->secondaryName.c_str();
    }

    const RendererStats& LayerRenderer::stats() const
    {
        return m_impl->stats;
    }
}
