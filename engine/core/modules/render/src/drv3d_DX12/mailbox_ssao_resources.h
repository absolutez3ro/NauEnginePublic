// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <memory>
#include <new>

#include "device_ssao_pass.h"
#include "mailbox_receive.h"
#include "shared_ssao_texture.h"

namespace drv3d_dx12
{
    // Owns every shared/native resource used by one mailbox producer. The producer drains both
    // adapters before destruction; borrowing these handles never transfers resource-state ownership.
    class MailboxSsaoResources
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

            ~RetiredSetup() { if (event) CloseHandle(event); }
        };

        static RetiredSetup*& retiredSetups()
        {
            // Render-thread owned. Retain uncertain submissions until their fence or device removal proves retirement safe.
            static RetiredSetup* pending = nullptr;
            return pending;
        }

        struct SharedBuffer
        {
            Microsoft::WRL::ComPtr<ID3D12Heap> heaps[2];
            Microsoft::WRL::ComPtr<ID3D12Resource> buffers[2];

            HRESULT init(ID3D12Device* primary, ID3D12Device* secondary, UINT64 bytes)
            {
                D3D12_RESOURCE_DESC desc = bufferDescription(bytes);
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
                const auto a = primary->GetResourceAllocationInfo(0, 1, &desc);
                const auto b = secondary->GetResourceAllocationInfo(0, 1, &desc);
                if (!a.SizeInBytes || !b.SizeInBytes || a.SizeInBytes == UINT64_MAX || b.SizeInBytes == UINT64_MAX)
                    return DXGI_ERROR_UNSUPPORTED;
                D3D12_HEAP_DESC heap = {};
                heap.SizeInBytes = a.SizeInBytes > b.SizeInBytes ? a.SizeInBytes : b.SizeInBytes;
                heap.Alignment = a.Alignment > b.Alignment ? a.Alignment : b.Alignment;
                heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
                HRESULT result = primary->CreateHeap(&heap, IID_PPV_ARGS(&heaps[0]));
                HANDLE handle = nullptr;
                if (SUCCEEDED(result)) result = primary->CreateSharedHandle(heaps[0].Get(), nullptr, GENERIC_ALL, nullptr, &handle);
                if (SUCCEEDED(result))
                {
                    result = secondary->OpenSharedHandle(handle, IID_PPV_ARGS(&heaps[1]));
                    CloseHandle(handle);
                }
                if (SUCCEEDED(result)) result = primary->CreatePlacedResource(heaps[0].Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffers[0]));
                if (SUCCEEDED(result)) result = secondary->CreatePlacedResource(heaps[1].Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffers[1]));
                if (SUCCEEDED(result))
                    for (const auto& buffer : buffers) buffer->SetName(L"SSAO shared mailbox");
                return result;
            }
        };

        SharedSsaoTexture m_depth[MAILBOX_IN_SLOTS];
        SharedSsaoTexture m_normals[MAILBOX_IN_SLOTS];
        SharedSsaoTexture m_ao[MAILBOX_OUT_SLOTS];
        SharedSsaoTexture m_id[MAILBOX_OUT_SLOTS];
        SharedBuffer m_mailbox;
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

        static HRESULT createBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state, const wchar_t* name,
                                    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = heapType;
            const auto desc = bufferDescription(bytes);
            const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                state, nullptr, IID_PPV_ARGS(&resource));
            if (SUCCEEDED(result)) resource->SetName(name);
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
                D3D12_RESOURCE_STATE_GENERIC_READ, L"SSAO mailbox initialization", upload);
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
            if (SUCCEEDED(result)) result = primary->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
            if (SUCCEEDED(result)) result = primary->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator));
            if (SUCCEEDED(result)) result = primary->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
            if (SUCCEEDED(result)) result = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
            HANDLE event = nullptr;
            if (SUCCEEDED(result))
            {
                event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event) result = HRESULT_FROM_WIN32(GetLastError());
            }
            if (SUCCEEDED(result))
            {
                const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition = {resource, 0, before, after};
                    list->ResourceBarrier(1, &barrier);
                };
                for (auto* buffer : {m_mailbox.buffers[0].Get(), m_predicate.Get()})
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
            if (SUCCEEDED(result)) result = done->SetEventOnCompletion(1, event);
            std::unique_ptr<RetiredSetup> retained;
            if (SUCCEEDED(result))
            {
                // Reserve failure-path storage before submitting; retaining work must not allocate after Signal fails.
                retained.reset(new (std::nothrow) RetiredSetup());
                if (!retained) result = E_OUTOFMEMORY;
                else
                {
                    retained->device = primary;
                    retained->queue = queue;
                    retained->allocator = allocator;
                    retained->list = list;
                    retained->done = done;
                    retained->upload = upload;
                    retained->targets[0] = m_mailbox.buffers[0];
                    retained->targets[1] = m_predicate;
                    retained->targets[2] = m_frameId;
                    for (UINT side = 0; side < 2; ++side) retained->sharedHeaps[side] = m_mailbox.heaps[side];
                }
            }
            if (SUCCEEDED(result))
            {
                ID3D12CommandList* lists[] = {list.Get()};
                queue->ExecuteCommandLists(1, lists);
                result = queue->Signal(done.Get(), 1);
                if (FAILED(result) && SUCCEEDED(primary->GetDeviceRemovedReason()))
                    result = queue->Signal(done.Get(), 1); // A second signal can still establish a retirement point.
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
            if (event) CloseHandle(event);
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
                    setup->signaled = SUCCEEDED(setup->queue->Signal(setup->done.Get(), 1));
                if (removed || (setup->signaled && setup->done->GetCompletedValue() >= 1))
                {
                    *link = setup->next;
                    delete setup;
                }
                else link = &setup->next;
            }
        }

        MailboxSsaoResources() = default;
        MailboxSsaoResources(const MailboxSsaoResources&) = delete;
        MailboxSsaoResources& operator=(const MailboxSsaoResources&) = delete;
        ~MailboxSsaoResources()
        {
            if (m_readbackMapped)
            {
                const D3D12_RANGE noWrite = {0, 0};
                m_readback->Unmap(0, &noWrite);
            }
        }

        HRESULT init(ID3D12Device* primary, ID3D12Device* secondary, const DeviceSsaoPassDesc& desc)
        {
            collectRetiredSetups();
            if (!primary || !secondary || !desc.width || !desc.height || m_frameId) return E_INVALIDARG;
            const UINT width = desc.inputWidth ? desc.inputWidth : desc.width;
            const UINT height = desc.inputHeight ? desc.inputHeight : desc.height;
            const UINT depthGroup = desc.linearDepth16 ? 8u : 4u;
            const UINT depthWidth = desc.packedFrameworkDepth ? (width + depthGroup - 1) / depthGroup : 0u;
            const UINT normalGroup = desc.packedNormals ? 8u : 4u;
            const UINT normalWidth = desc.packedFrameworkNormals ? (width + normalGroup - 1) / normalGroup : 0u;
            HRESULT result = S_OK;
            for (UINT i = 0; i < IN_SLOTS && SUCCEEDED(result); ++i)
            {
                result = m_depth[i].init(primary, secondary, width, height,
                    desc.linearDepth16 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT, depthWidth, DXGI_FORMAT_R32G32B32A32_UINT);
                if (SUCCEEDED(result) && !desc.reconstructNormals)
                    result = m_normals[i].init(primary, secondary, width, height,
                        desc.packedNormals ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_FLOAT, normalWidth, DXGI_FORMAT_R32G32B32A32_UINT);
                wchar_t name[64] = {};
                std::swprintf(name, 64, L"SSAO mailbox depth input %u", i);
                m_depth[i].setName(name);
                std::swprintf(name, 64, L"SSAO mailbox normal input %u", i);
                m_normals[i].setName(name);
            }
            for (UINT j = 0; j < OUT_SLOTS && SUCCEEDED(result); ++j)
            {
                result = m_ao[j].init(primary, secondary, desc.width, desc.height, DXGI_FORMAT_R8_UNORM);
                if (SUCCEEDED(result)) result = m_id[j].init(primary, secondary, 1, 1, DXGI_FORMAT_R32_UINT);
                wchar_t name[64] = {};
                std::swprintf(name, 64, L"SSAO mailbox AO output %u", j);
                m_ao[j].setName(name);
                std::swprintf(name, 64, L"SSAO mailbox frame ID output %u", j);
                m_id[j].setName(name);
            }
            if (SUCCEEDED(result)) result = m_mailbox.init(primary, secondary, MAILBOX_BYTES);
            if (SUCCEEDED(result)) result = createBuffer(primary, MAILBOX_BYTES, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, L"SSAO local mailbox predicate", m_predicate);
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
                if (SUCCEEDED(result)) m_frameId->SetName(L"SSAO received frame ID");
            }
            if (SUCCEEDED(result)) result = createBuffer(primary, READBACK_SLOTS * READBACK_STRIDE, D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST, L"SSAO received frame ID readback ring", m_readback);
            if (SUCCEEDED(result)) result = initializeEmptyMailbox(primary);
            if (SUCCEEDED(result))
            {
                const D3D12_RANGE read = {0, static_cast<SIZE_T>(READBACK_SLOTS * READBACK_STRIDE)};
                void* mapped = nullptr;
                result = m_readback->Map(0, &read, &mapped);
                if (SUCCEEDED(result)) m_readbackMapped = static_cast<const uint8_t*>(mapped);
            }
            return result;
        }

        FrameworkSsaoTextures input(UINT i) const
        {
            return {m_normals[i].textures[1].Get(), m_depth[i].textures[1].Get(), nullptr,
                {m_normals[i].heaps[1].Get(), m_depth[i].heaps[1].Get(), nullptr}};
        }
        FrameworkSsaoTextures shared(UINT i, UINT j) const
        {
            auto textures = input(i);
            textures.result = m_ao[j].textures[1].Get();
            textures.heaps[2] = m_ao[j].heaps[1].Get();
            return textures;
        }
        ID3D12Resource* inputDepthPrimary(UINT i) const { return m_depth[i].textures[0].Get(); }
        ID3D12Resource* inputNormalsPrimary(UINT i) const { return m_normals[i].textures[0].Get(); }
        ID3D12Resource* outputAoPrimary(UINT j) const { return m_ao[j].textures[0].Get(); }
        ID3D12Resource* outputIdPrimary(UINT j) const { return m_id[j].textures[0].Get(); }
        ID3D12Resource* outputAoSecondary(UINT j) const { return m_ao[j].textures[1].Get(); }
        ID3D12Resource* outputIdSecondary(UINT j) const { return m_id[j].textures[1].Get(); }
        ID3D12Resource* mailboxPrimary() const { return m_mailbox.buffers[0].Get(); }
        ID3D12Resource* mailboxSecondary() const { return m_mailbox.buffers[1].Get(); }
        ID3D12Resource* predicate() const { return m_predicate.Get(); }
        ID3D12Resource* frameId() const { return m_frameId.Get(); }
        ID3D12Resource* readback() const { return m_readback.Get(); }
        const uint8_t* mappedReadback() const { return m_readbackMapped; }
    };
}
