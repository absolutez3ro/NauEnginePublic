// device_resource.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// Native resources bound to an explicitly selected D3D12 device.
#pragma once

#include <wrl/client.h>

#include <cstdint>  // Required by the bundled D3D12 declarations.

#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    /**
     * @brief Owns a native resource and its explicitly selected device outside the primary resource manager.
     *
     * Use borrowed handles only with queues and descriptors from getDevice(). The caller must synchronize
     * access, track resource transitions and finish GPU work before reset or destruction. Release resources
     * before driver shutdown or device recovery. This class does not submit commands or wait for the GPU.
     */
    class DeviceResource
    {
    public:
        DeviceResource() = default;
        DeviceResource(const DeviceResource&) = delete;
        DeviceResource(DeviceResource&&) = delete;
        DeviceResource& operator=(const DeviceResource&) = delete;
        DeviceResource& operator=(DeviceResource&&) = delete;
        ~DeviceResource() = default;

    public:
        /**
         * @brief Creates a single-mip RGBA8 render target in the COMMON state.
         * @param device Owning device; nullptr is rejected without falling back to the primary GPU.
         * @param width Texture width in pixels.
         * @param height Texture height in pixels.
         * @return S_OK on success, E_INVALIDARG for invalid input, DXGI_ERROR_INVALID_CALL if already
         * initialized, or the native D3D12 error. Failure leaves this object unchanged.
         */
        HRESULT createRenderTarget(ID3D12Device* device, UINT width, UINT height);

        /**
         * @brief Creates a buffer with the initial state required by its heap type.
         * @param device Owning device; nullptr is rejected without falling back to the primary GPU.
         * @param size Buffer size in bytes; must be nonzero.
         * @param heapType DEFAULT, UPLOAD or READBACK, using COMMON, GENERIC_READ or COPY_DEST respectively.
         * @return S_OK on success, E_INVALIDARG for invalid input, DXGI_ERROR_INVALID_CALL if already
         * initialized, or the native D3D12 error. Failure leaves this object unchanged.
         */
        HRESULT createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType);

        /**
         * @brief Releases the resource before its device reference; safe to call on an empty object.
         * @pre The caller has completed all GPU work that uses this resource.
         */
        void reset();

        /**
         * @brief Returns the resource's explicitly retained device.
         * @return A borrowed device pointer, or nullptr when empty.
         */
        ID3D12Device* getDevice() const;

        /**
         * @brief Returns the native resource without transferring ownership.
         * @return A borrowed resource pointer, or nullptr when empty.
         */
        ID3D12Resource* getResource() const;

        /**
         * @brief Reports the creation state; does not track subsequent command-list transitions.
         * @return The initial resource state, or COMMON when empty.
         */
        D3D12_RESOURCE_STATES getInitialState() const;

    private:
        HRESULT create(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clearValue);

    private:
        // COM references require AddRef/Release, so use the existing DX12 ComPtr convention here.
        // Reverse destruction order releases the resource before its explicit device reference.
        Microsoft::WRL::ComPtr<ID3D12Device> m_owner;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
        D3D12_RESOURCE_STATES m_initialState = D3D12_RESOURCE_STATE_COMMON;
    };
}  // namespace drv3d_dx12
