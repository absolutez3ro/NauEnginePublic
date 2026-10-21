// device_fullscreen_pass.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
// A self-contained fullscreen render pass that runs on an explicitly selected D3D12 device (GPU 1 for MG-10).
#pragma once

#include <EASTL/shared_ptr.h>
#include <EASTL/unique_ptr.h>
#include <wrl/client.h>

#include <cstdint>  // Required by the bundled D3D12 declarations.

#include "device_resource.h"
#include "nau/directx/d3d12.h"

namespace drv3d_dx12
{
    /**
     * @brief HLSL of the built-in MG-10 test pass; compile entry points "vsMain" (vs_5_0) and "psMain" (ps_5_0).
     *
     * The vertex shader emits one fullscreen triangle from SV_VertexID. The pixel shader draws a checkerboard:
     * odd pixels (x ^ y) get the constant buffer color, even pixels get its inverse (rgb only, alpha 1).
     * A checkerboard proves that the pixel shader really ran with SV_Position, unlike a clear color would.
     */
    inline constexpr char FULLSCREEN_TEST_PASS_HLSL[] = R"hlsl(
cbuffer PassConstants : register(b0)
{
    float4 g_color;
};

struct VsOutput
{
    float4 position : SV_Position;
};

VsOutput vsMain(uint vertexId : SV_VertexID)
{
    // Vertices (0,0) (2,0) (0,2) in uv space cover the whole viewport with a single triangle.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VsOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 psMain(VsOutput input) : SV_Target
{
    uint2 pixel = (uint2)input.position.xy;
    bool odd = ((pixel.x ^ pixel.y) & 1) != 0;
    return odd ? g_color : float4(1.0 - g_color.rgb, 1.0);
}
)hlsl";

    /**
     * @brief Color written by the built-in test pass. Matches the cbuffer layout of FULLSCREEN_TEST_PASS_HLSL.
     */
    struct FullscreenPassColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    /**
     * @brief Everything init() needs. The shader bytecode is only read during init().
     */
    struct DeviceFullscreenPassDesc
    {
        eastl::shared_ptr<IDeviceResourceAllocator> allocator;  ///< Retained until all submitted GPU work ends.
        UINT width = 0;
        UINT height = 0;
        D3D12_SHADER_BYTECODE vertexShader = {};  ///< DXBC or DXIL; no input layout, vertices come from SV_VertexID.
        D3D12_SHADER_BYTECODE pixelShader = {};   ///< Writes SV_Target0 as R8G8B8A8_UNORM; may read b0 (16 bytes of color).
    };

    /**
     * @brief Draws one fullscreen triangle into its own render target on an explicitly selected device.
     *
     * This is the "pass owner" for the secondary GPU. It owns everything the pass needs, all created on
     * the device passed to init(): a DIRECT queue, command allocators and list, a fence, an RTV heap, a root signature,
     * a graphics PSO, a render target, a constant buffer and an optional readback buffer. It does not touch the
     * primary device or the ShaderProgramDatabase. Resource memory comes from the supplied allocator.
     *
     * Synchronization: up to FRAME_COUNT submissions can be in flight. execute() never waits for the GPU unless it
     * needs to reuse the allocator and constant buffer slot of a submission that is still running. The render
     * target is left in D3D12_RESOURCE_STATE_COMMON after every execute(), which is the state the MG-04 transfer
     * channel expects from send_from_secondary(); the fence from getFence() and the value from
     * getLastSubmittedValue() form its source_ready sync point after a successful execute().
     * Before overwriting or releasing a target read by another queue, enqueue waitForExternalWork() with its
     * release fence. Retirement cannot track consumers whose completion was never registered with the pass.
     *
     * Thread safety: none. Call everything from the render thread. reset() waits for the GPU on the CPU, call it
     * before driver shutdown or device recovery.
     */
    class DeviceFullscreenPass
    {
    public:
        /// Submissions that may be in flight at once; each has its own allocator and constant buffer slot.
        static constexpr UINT FRAME_COUNT = 2;
        /// Default timeout of explicit CPU waits. Destruction itself never waits for GPU completion.
        static constexpr DWORD WAIT_TIMEOUT_MS = 10000;

    public:
        DeviceFullscreenPass();
        DeviceFullscreenPass(const DeviceFullscreenPass&) = delete;
        DeviceFullscreenPass(DeviceFullscreenPass&&) = delete;
        DeviceFullscreenPass& operator=(const DeviceFullscreenPass&) = delete;
        DeviceFullscreenPass& operator=(DeviceFullscreenPass&&) = delete;
        ~DeviceFullscreenPass();

    public:
        /**
         * @brief Creates all objects on desc.allocator->getDevice().
         * @param desc Allocator, target size and shader bytecode.
         * @return S_OK on success, E_INVALIDARG for a null device, zero or too large size or empty shaders,
         * DXGI_ERROR_INVALID_CALL if already initialized, or the native D3D12/Win32 error.
         * Any failure releases everything that was created and leaves the object empty.
         */
        HRESULT init(const DeviceFullscreenPassDesc& desc);

        /**
         * @brief Waits for submitted work and releases everything. Safe to call on an empty object.
         *
         * On timeout or a recoverable wait error, returns the error and retains all objects for a later retry.
         * Only completion or confirmed device removal permits releasing GPU memory. Destruction polls once,
         * then retires unfinished state without waiting. Its allocator and external fences remain owned.
         * Callers should handle reset() errors and stop submitting work to an unavailable secondary path.
         */
        HRESULT reset(DWORD timeoutMs = WAIT_TIMEOUT_MS);

        /**
         * @brief Polls retired passes without waiting; releases completed or removed-device state.
         * @return Number of states still waiting for GPU completion or device removal.
         *
         * Call on the render thread each frame and during shutdown, before unloading Render or its allocator
         * code. init() and execute() also collect. No worker thread is involved. If an external dependency never
         * completes and the device stays alive, its state remains pinned until process exit; shutdown must not
         * spin on this count. Never fake completion by signaling a consumer's fence from the producer.
         */
        static size_t collectRetiredResources();

        /**
         * @brief Sets the color used by the next execute() calls. Copied into a per-submission constant buffer.
         */
        void setColor(const FullscreenPassColor& color);

        /**
         * @brief Records and submits the pass: draw, then optionally copy the target into the readback buffer.
         * @param copyToReadback Also copy the target into the readback buffer for readPixels().
         * @return S_OK once submitted. The GPU may still be running. DXGI_ERROR_INVALID_CALL if not initialized,
         * HRESULT_FROM_WIN32(ERROR_TIMEOUT) if the previous use of this submission slot never finished, or the
         * device removed reason / native error.
         */
        HRESULT execute(bool copyToReadback);

        /**
         * @brief Blocks the CPU until everything submitted so far has finished on the GPU.
         * @return S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT) on timeout, or the device removed reason.
         */
        HRESULT waitForCompletion(DWORD timeoutMs = WAIT_TIMEOUT_MS);

        /**
         * @brief Enqueues a GPU wait before the next draw, for example the transfer channel's sourceReleased fence.
         * @param fence Fence opened on this device; the pass retains it through completion. It must not depend
         * on a future submission of this pass.
         * @param value Value signaled by the consumer when the render target may be overwritten.
         */
        HRESULT waitForExternalWork(ID3D12Fence* fence, UINT64 value);

        /**
         * @brief Copies the pixels of the last execute(true) into a tightly packed RGBA8 buffer.
         * @param destination Receives width * height * 4 bytes, rows without padding.
         * @param destinationSize Size of destination in bytes; must be at least width * height * 4.
         * @return S_OK, E_INVALIDARG for a null or too small destination, DXGI_ERROR_INVALID_CALL if nothing was
         * requested with copyToReadback, DXGI_ERROR_WAS_STILL_DRAWING if the GPU has not finished it yet (call
         * waitForCompletion() first), or the native Map error.
         */
        HRESULT readPixels(uint8_t* destination, size_t destinationSize) const;

        /**
         * @brief Fence signaled after each execute(). Borrowed; valid until reset().
         */
        ID3D12Fence* getFence() const;

        /**
         * @brief Latest queued draw or external-wait completion value. 0 if nothing was submitted.
         */
        UINT64 getLastSubmittedValue() const;

        /**
         * @brief Render target the pass draws into, left in COMMON state between executions. Borrowed.
         */
        ID3D12Resource* getRenderTarget() const;

        /**
         * @brief Constant buffer, root signature and PSO are exposed for ownership checks in tests only.
         */
        ID3D12PipelineState* getPipelineState() const;
        ID3D12RootSignature* getRootSignature() const;
        ID3D12Resource* getConstantBuffer() const;

        /**
         * @brief Returns the device given to init(), or nullptr when empty.
         */
        ID3D12Device* getDevice() const;

    private:
        class State;
        // Intrusive process-lifetime list: pending GPU work is never released by static destruction.
        static State* s_retiredStates;
        eastl::unique_ptr<State> m_state;
        FullscreenPassColor m_color;
    };
}  // namespace drv3d_dx12
