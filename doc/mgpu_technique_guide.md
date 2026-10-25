# Integrating a technique with MGPU

This guide covers the path GPU 0 inputs → GPU 1 processing → GPU 0 resolve. [SSAO](ambient_occlusion.md) is the complete example; the [fullscreen sample](mgpu_fullscreen_sample.md) demonstrates rendering and output transfer without scene inputs.

## Check secondary availability

Enable `dx12.multiGpu` in application properties. Keep `secondaryWarpForTesting=false` for hardware selection, or explicitly enable it for functional WARP testing. Preserve other properties in the configuration file.

On the render thread, after driver initialization:

```cpp
if (!d3d::is_multi_gpu_available())
{
    NAU_LOG_INFO("MGPU fallback: {}", d3d::get_multi_gpu_fallback_reason());
    // Run the technique on the primary GPU.
}
```

`get_device(SECONDARY_GPU)` returns a borrowed native device; do not release it. Both discovery and health checks can disable secondary access. Even when available, channel/format creation can fail. Report failed HRESULTs through `report_secondary_gpu_failure`; handle a busy slot separately. See [device and fallback details](multi_gpu_dx12.md).

## Create resources on GPU 1

Keep native DX12 objects inside Render. Graphics uses public producer APIs such as [`multi_gpu_ssao.h`](../engine/core/modules/render/include/nau/3d/multi_gpu_ssao.h) and [`multi_gpu_fullscreen.h`](../engine/core/modules/render/include/nau/3d/multi_gpu_fullscreen.h). A new technique can follow their opaque-handle interface.

Inside the backend, use Nau's resource pool through `NauDeviceResourceAllocator`. Example owner fields and initialization:

```cpp
eastl::shared_ptr<drv3d_dx12::NauDeviceResourceAllocator> m_allocator;
drv3d_dx12::DeviceResource m_normals;
drv3d_dx12::DeviceResource m_result;
```

```cpp
m_allocator = eastl::make_shared<drv3d_dx12::NauDeviceResourceAllocator>();
HRESULT result = m_allocator->init(
    static_cast<ID3D12Device*>(d3d::get_device(d3d::SECONDARY_GPU)));
if (SUCCEEDED(result))
{
    result = m_normals.createTexture(m_allocator.get(), width, height, DXGI_FORMAT_R16G16_FLOAT);
}
if (SUCCEEDED(result))
{
    result = m_result.createTexture(m_allocator.get(), width, height, DXGI_FORMAT_R8_UNORM,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
}
```

The allocator must outlive its resources, and resources must outlive all GPU use. Initialize a placed render target with clear/discard or a full copy before drawing. Create queues, root signatures, PSOs and descriptor heaps on the secondary device; only shader bytecode can be shared directly. The transport owns the cross-adapter shared heap, so techniques do not need a separate transport implementation.

## Transfer inputs and outputs

Create channels once with [`TransferChannelDesc`](../engine/core/modules/render/include/nau/3d/dag_drv3d_multi_gpu.h). Set direction, size and format, or use `payload=Buffer` and `byteSize`. Check handles and `is_transfer_format_supported`.

| Direction | Send | Receive |
| --- | --- | --- |
| 0 → 1 | `send_from_primary(channel, engineSource)` returns ticket | `receive_on_secondary(channel, ticket, nativeDestination, destinationFree)` returns ready fence/value |
| 1 → 0 | `send_from_secondary(channel, nativeSource, sourceReady)` returns ticket and sourceReleased | `receive_on_primary(channel, ticket, engineDestination)` returns bool |

Follow [`submitSsao` / `tryReceiveSsao`](../engine/core/modules/render/src/drv3d_DX12/multi_gpu_ssao.cpp) for ordering:

1. Send primary inputs after their producing work.
2. Pass the previous use of the native destination as `destinationFree`; the receiving COPY queue must not overwrite inputs still read by a DIRECT queue.
3. Make the technique's DIRECT queue wait for all input-ready points. Execute the pass and signal output readiness.
4. Send the output with that ready point. Retain `sourceReleased` and wait for it before overwriting the output.
5. Retain the output ticket across frames. Poll `is_transfer_ready` before receiving it into the alternate tracked engine destination. Publish only a completed older result; if it is not ready, reuse the existing map. The receive still synchronizes its local destination copy.
6. After consuming a ready result, immediately try to submit current inputs in the same late node; waiting for the next frame's early submit node wastes a submission opportunity. Skip while busy and submit each source frame at most once, including when an early job finishes in the same frame. SSAO keeps one outstanding job, three shared slots and two persistent primary AO maps.

Native endpoints must be in `D3D12_RESOURCE_STATE_COMMON` when copying begins. The technique owns its DIRECT-queue transitions. Engine textures must be tracked and use `TEXCF_RTARGET`, `TEXCF_UNORDERED` or `TEXCF_UPDATE_DESTINATION`. Payload size, layout and endpoint formats must match the channel's supported copy contract.

Ticket zero means no transfer was sent, including slot pressure; it never identifies a ready result. Handle busy separately from failure. AO reuses its latest valid map while busy, computes locally when no map exists, and invalidates history on resize/mode/device changes. Choose a history policy suitable for the new technique. SceneBaseSample sends only prepass depth; its GPU 1 pass reconstructs local normals using `SsaoProducerDesc::normalBytecode`. The legacy normal-input channel remains available when this shader is omitted. The sample defaults to half-resolution R32F input (`depthIsFloat=true`) and R8 AO, with depth-aware upsampling on GPU 0. Submission occurs between `z-prepass` and `fill_gbuffer`; reception polls after CSM, before lighting.

## Minimal fullscreen producer

With loaded `const Shader* vertex` and `const Shader* pixel`, initialize a producer and a GPU 0 destination of size 960×540, format `TEXFMT_R8G8B8A8 | TEXCF_RTARGET`:

```cpp
d3d::mgpu::FullscreenProducerDesc desc;
desc.width = 960;
desc.height = 540;
desc.vertexBytecode = vertex->bytecode.data();
desc.vertexBytecodeSize = vertex->bytecode.size();
desc.pixelBytecode = pixel->bytecode.data();
desc.pixelBytecodeSize = pixel->bytecode.size();
m_producer = d3d::mgpu::createFullscreenProducer(desc);
```

During rendering, with a valid producer and destination:

```cpp
const float constants[4] = {static_cast<float>(phase), 960.0f, 540.0f, 0.0f};
const auto result = d3d::mgpu::renderFullscreen(m_producer, constants, m_destination);
if (result == d3d::mgpu::FullscreenResult::submitted)
{
    d3d::stretch_rect(m_destination, backBuffer);
}
```

The full [`MultiGpuDemo`](../engine/core/modules/graphics/src/render_window/multi_gpu_demo.cpp) handles busy, failure, initial fallback and destruction. `submitted` means work was queued; fences ensure safe subsequent reading. This producer has only four constants; a scene-dependent technique needs its own input/constant contract, like [`SsaoConstants`](../engine/core/modules/render/include/nau/3d/multi_gpu_ssao_types.h).

## Lifetime, depth and profiling

| Area | Requirement or limitation |
| --- | --- |
| Depth | Copy depth to the other device. AO supports one-plane reversed-Z DEPTH32 to R32_FLOAT and reduced R32F input. Combined depth-stencil is not supported by this channel. |
| Delay | SSAO polls before lighting: same-frame completion is permitted if already ready, otherwise it reuses older history. Slower secondary execution increases age; history has no reprojection or age bound. |
| Shutdown/resize | Drain channels, then release passes/resources, then the allocator. Native passes can retire unfinished work; channel destruction can block the CPU. |
| Collection | Graphics calls `collectFullscreenResources()` each frame and before driver teardown; this also collects SSAO retirement. A standalone owner must arrange equivalent collection. |
| Recovery | Destroy owners before driver recovery and recreate them afterward. Do not retain borrowed pointers across recovery. |
| ABI | `receive_on_secondary` includes `destination_free`; `TransferChannelDesc` includes an optional profiler; `SsaoProducerDesc` can include normal-reconstruction bytecode and `depthIsFloat`. Rebuild consuming modules together when adopting these structs/APIs. |

For interactive AO inspection, disable `sample.mgpuDemo.enabled` and run:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device warp -Mode lit -Interactive -CaptureFrame 0
```

F6 switches the fullscreen path. In AO, F7 cycles lit/map/off and F8 requests primary/secondary. The AO algorithm is adapted from Bogdanov M.K.; see its [description and attribution](ambient_occlusion.md).

Use `-Profile -CaptureFrame 0` with the AO runner, or `NAU_GPU_PROFILER=1` when launching the executable directly. Run primary and secondary configurations separately: the CSV mode label comes from startup settings. A new channel needs an `ITransferProfiler` observer to measure its COPY lists; enabling the profiler alone does not instrument every channel. The fullscreen sample currently has no such secondary instrumentation.

Each queue uses its own timestamp frequency. `/src` and `/dst` time local copies, `/sum` adds their durations, and calibrated `/span` covers the interval from the source copy start to destination copy end. COPY support is optional; missing timers produce warnings, not zero measurements. See [commands, interpretation and WARP results](ambient_occlusion_measurements.md).

WARP validates the path but does not predict hardware speedup. Presentation defaults to vsync; use `video.vsync=false`, `video.adaptive_vsync=false` or the runner's `-NoVsync` for comparisons. Inspect GPU CSV timings as well as frame cadence. SSAO polls source-copy completion before reception, so primary `Frame` no longer waits for an unfinished secondary job. The immediate fullscreen path still can wait. Blocking shutdown waits, stale history and unresolved asynchronous-loading shutdown failures remain limitations.
