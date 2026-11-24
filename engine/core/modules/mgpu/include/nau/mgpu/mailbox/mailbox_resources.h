// mailbox_resources.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <memory>
#include <new>

#include "mailbox_receive.h"
#include "nau/mgpu/bridge/device_context.h"
#include "shared_texture.h"

namespace nau::mgpu
{
    /**
     * @brief Owns the shared and native resources of one mailbox producer.
     * The producer drains both adapters before destruction. Borrowing a handle does not transfer
     * responsibility for its resource state.
     */
    struct MailboxDescription
    {
        struct Texture
        {
            UINT width = 0, height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            UINT peerWidth = 0;
            DXGI_FORMAT peerFormat = DXGI_FORMAT_UNKNOWN;
        };
        static constexpr UINT MAX_INPUTS = 4;
        Texture inputs[MAX_INPUTS];
        UINT inputCount = 0;
        Texture output;
    };

    class MailboxResources
    {
        struct RetiredSetup
        {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            Microsoft::WRL::ComPtr<ID3D12Fence> done;
            Microsoft::WRL::ComPtr<ID3D12Resource> upload;
            Microsoft::WRL::ComPtr<ID3D12Resource> targets[3];
            Microsoft::WRL::ComPtr<ID3D12Heap> sharedHeaps[2];
            HANDLE event = nullptr;
            bool signaled = false;
            RetiredSetup* next = nullptr;

            ~RetiredSetup()
            {
                if (event)
                {
                    CloseHandle(event);
                }
            }
        };

        static RetiredSetup*& retiredSetups()
        {
            // Render-thread owned. Retain uncertain submissions until their fence or device removal proves retirement safe.
            static RetiredSetup* pending = nullptr;
            return pending;
        }

        std::shared_ptr<DeviceContext> m_primary, m_secondary;
        SharedTexture m_inputs[MAILBOX_IN_SLOTS][MailboxDescription::MAX_INPUTS];
        SharedTexture m_payload[MAILBOX_OUT_SLOTS];
        SharedTexture m_id[MAILBOX_OUT_SLOTS];
        SharedResource m_mailbox;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_predicate;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_frameId;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
        const uint8_t* m_readbackMapped = nullptr;

        static D3D12_RESOURCE_DESC bufferDescription(UINT64 bytes)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return desc;
        }

        static HRESULT createBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = heapType;
            const auto desc = bufferDescription(bytes);
            const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                   state, nullptr, IID_PPV_ARGS(&resource));
            if (SUCCEEDED(result))
            {
                resource->SetName(name);
            }
            return result;
        }

        HRESULT initializeEmptyMailbox(ID3D12Device* primary)
        {
            // This one-time setup is complete before either render queue can consume the mailbox.
            // No initialization submit or CPU wait occurs in the per-frame exchange.
            Microsoft::WRL::ComPtr<ID3D12Resource> upload;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            Microsoft::WRL::ComPtr<ID3D12Fence> done;
            constexpr UINT64 uploadBytes = 2 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
            HRESULT result = createBuffer(primary, uploadBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, L"MGPU mailbox initialization", upload);
            if (SUCCEEDED(result))
            {
                const D3D12_RANGE noRead = {0, 0};
                void* mapped = nullptr;
                result = upload->Map(0, &noRead, &mapped);
                if (SUCCEEDED(result))
                {
                    std::memset(mapped, 0, static_cast<size_t>(uploadBytes));
                    const D3D12_RANGE written = {0, static_cast<SIZE_T>(uploadBytes)};
                    upload->Unmap(0, &written);
                }
            }
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (SUCCEEDED(result))
            {
                queue = m_primary->graphicsQueue();
                result = queue ? S_OK : E_FAIL;
            }
            if (SUCCEEDED(result))
            {
                result = primary->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator));
            }
            if (SUCCEEDED(result))
            {
                result = primary->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
            }
            if (SUCCEEDED(result))
            {
                result = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
            }
            HANDLE event = nullptr;
            if (SUCCEEDED(result))
            {
                event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event)
                {
                    result = HRESULT_FROM_WIN32(GetLastError());
                }
            }
            if (SUCCEEDED(result))
            {
                const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition = {resource, 0, before, after};
                    list->ResourceBarrier(1, &barrier);
                };
                for (auto* buffer : {m_mailbox.resource(0), m_predicate.Get()})
                {
                    transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->CopyBufferRegion(buffer, 0, upload.Get(), 0, MAILBOX_BYTES);
                    transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                }
                transition(m_frameId.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
                source.pResource = upload.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint.Offset = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                source.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
                destination.pResource = m_frameId.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                transition(m_frameId.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                result = list->Close();
            }
            if (SUCCEEDED(result))
            {
                result = done->SetEventOnCompletion(1, event);
            }
            std::unique_ptr<RetiredSetup> retained;
            if (SUCCEEDED(result))
            {
                // Reserve failure-path storage before submitting; retaining work must not allocate after Signal fails.
                retained.reset(new(std::nothrow) RetiredSetup());
                if (!retained)
                {
                    result = E_OUTOFMEMORY;
                }
                else
                {
                    retained->device = primary;
                    retained->queue = queue;
                    retained->allocator = allocator;
                    retained->list = list;
                    retained->done = done;
                    retained->upload = upload;
                    retained->targets[0] = m_mailbox.resource(0);
                    retained->targets[1] = m_predicate;
                    retained->targets[2] = m_frameId;
                    for (UINT side = 0; side < 2; ++side)
                    {
                        retained->sharedHeaps[side] = m_mailbox.heap(side);
                    }
                }
            }
            if (SUCCEEDED(result))
            {
                ID3D12CommandList* lists[] = {list.Get()};
                queue->ExecuteCommandLists(1, lists);
                result = queue->Signal(done.Get(), 1);
                if (FAILED(result) && SUCCEEDED(primary->GetDeviceRemovedReason()))
                {
                    result = queue->Signal(done.Get(), 1);  // A second signal can still establish a retirement point.
                }
                retained->signaled = SUCCEEDED(result);
                if (SUCCEEDED(result) && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
                {
                    const DWORD error = GetLastError();
                    result = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
                }
                if (FAILED(result) && SUCCEEDED(primary->GetDeviceRemovedReason()) &&
                    (!retained->signaled || done->GetCompletedValue() < 1))
                {
                    retained->event = event;
                    event = nullptr;
                    retained->next = retiredSetups();
                    retiredSetups() = retained.release();
                }
            }
            if (event)
            {
                CloseHandle(event);
            }
            return result;
        }

    public:
        static constexpr UINT IN_SLOTS = MAILBOX_IN_SLOTS;
        static constexpr UINT OUT_SLOTS = MAILBOX_OUT_SLOTS;
        static constexpr UINT READBACK_SLOTS = 32;
        static constexpr UINT64 READBACK_STRIDE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        static constexpr UINT64 MAILBOX_BYTES = OUT_SLOTS * sizeof(uint64_t);

        static void collectRetiredSetups()
        {
            auto** link = &retiredSetups();
            while (*link)
            {
                auto* setup = *link;
                const bool removed = FAILED(setup->device->GetDeviceRemovedReason());
                if (!removed && !setup->signaled)
                {
                    setup->signaled = SUCCEEDED(setup->queue->Signal(setup->done.Get(), 1));
                }
                if (removed || (setup->signaled && setup->done->GetCompletedValue() >= 1))
                {
                    *link = setup->next;
                    delete setup;
                }
                else
                {
                    link = &setup->next;
                }
            }
        }

        MailboxResources() = default;
        MailboxResources(const MailboxResources&) = delete;
        MailboxResources& operator=(const MailboxResources&) = delete;
        ~MailboxResources()
        {
            if (m_readbackMapped)
            {
                const D3D12_RANGE noWrite = {0, 0};
                m_readback->Unmap(0, &noWrite);
            }
        }

        HRESULT init(const std::shared_ptr<DeviceContext>& primaryContext,
                     const std::shared_ptr<DeviceContext>& secondaryContext,
                     const MailboxDescription& desc)
        {
            collectRetiredSetups();
            if (!primaryContext || !secondaryContext || !desc.inputCount ||
                desc.inputCount > MailboxDescription::MAX_INPUTS || m_frameId)
            {
                return E_INVALIDARG;
            }
            m_primary = primaryContext;
            m_secondary = secondaryContext;
            auto* primary = m_primary->device();
            HRESULT result = S_OK;
            for (UINT slot = 0; slot < IN_SLOTS && SUCCEEDED(result); ++slot)
            {
                for (UINT input = 0; input < desc.inputCount && SUCCEEDED(result); ++input)
                {
                    const auto& texture = desc.inputs[input];
                    result = m_inputs[slot][input].init(*m_primary, *m_secondary, texture.width, texture.height,
                                                        texture.format, texture.peerWidth, texture.peerFormat);
                }
            }
            for (UINT slot = 0; slot < OUT_SLOTS && SUCCEEDED(result); ++slot)
            {
                result = m_payload[slot].init(*m_primary, *m_secondary, desc.output.width, desc.output.height, desc.output.format);
                if (SUCCEEDED(result))
                {
                    result = m_id[slot].init(*m_primary, *m_secondary, 1, 1, DXGI_FORMAT_R32_UINT);
                }
            }
            if (SUCCEEDED(result))
            {
                const auto description = bufferDescription(MAILBOX_BYTES);
                result = m_mailbox.init(*m_primary, *m_secondary, description);
            }
            if (SUCCEEDED(result))
            {
                result = createBuffer(primary, MAILBOX_BYTES, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COMMON, L"MGPU local mailbox predicate", m_predicate);
            }
            if (SUCCEEDED(result))
            {
                D3D12_RESOURCE_DESC id = {};
                id.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                id.Width = id.Height = id.DepthOrArraySize = id.MipLevels = id.SampleDesc.Count = 1;
                id.Format = DXGI_FORMAT_R32_UINT;
                D3D12_HEAP_PROPERTIES heap = {};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                result = primary->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &id,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_frameId));
                if (SUCCEEDED(result))
                {
                    m_frameId->SetName(L"MGPU received frame ID");
                }
            }
            if (SUCCEEDED(result))
            {
                result = createBuffer(primary, READBACK_SLOTS * READBACK_STRIDE, D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST, L"MGPU received frame ID readback ring", m_readback);
            }
            if (SUCCEEDED(result))
            {
                result = initializeEmptyMailbox(primary);
            }
            if (SUCCEEDED(result))
            {
                const D3D12_RANGE read = {0, static_cast<SIZE_T>(READBACK_SLOTS * READBACK_STRIDE)};
                void* mapped = nullptr;
                result = m_readback->Map(0, &read, &mapped);
                if (SUCCEEDED(result))
                {
                    m_readbackMapped = static_cast<const uint8_t*>(mapped);
                }
            }
            return result;
        }

        ID3D12Resource* input(UINT slot, UINT channel, UINT side) const
        {
            return m_inputs[slot][channel].resource(side);
        }
        ID3D12Resource* output(UINT slot, UINT side) const
        {
            return m_payload[slot].resource(side);
        }
        ID3D12Resource* outputId(UINT slot, UINT side) const
        {
            return m_id[slot].resource(side);
        }
        ID3D12Resource* mailboxPrimary() const
        {
            return m_mailbox.resource(0);
        }
        ID3D12Resource* mailboxSecondary() const
        {
            return m_mailbox.resource(1);
        }
        ID3D12Resource* predicate() const
        {
            return m_predicate.Get();
        }
        ID3D12Resource* frameId() const
        {
            return m_frameId.Get();
        }
        ID3D12Resource* readback() const
        {
            return m_readback.Get();
        }
        const uint8_t* mappedReadback() const
        {
            return m_readbackMapped;
        }
    };
}  // namespace nau::mgpu
