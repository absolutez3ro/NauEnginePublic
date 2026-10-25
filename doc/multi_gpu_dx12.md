# DX12 multi-GPU devices and fallback

Set `dx12.multiGpu=true` in the application's global properties to enable creation of a separate secondary D3D12 device. SceneBaseSample exposes this in `config/app.json`; the default is false. A Dagor settings provider can alternatively set `multiGpu:b=yes` in the `dx12` DataBlock. An explicit application property takes precedence.

Primary adapter selection remains unchanged. Normal secondary selection excludes the primary LUID and software/WARP adapters and respects driver preferences and the configured feature level. Failed device creation tries the next candidate. Exhaustion logs a fallback reason and keeps primary rendering available. With the feature disabled, the driver does no additional secondary enumeration or creation.

## Native access

The declarations are in `nau/3d/dag_drv3d.h`:

```cpp
d3d::GpuId primary = d3d::PRIMARY_GPU;
d3d::GpuId secondary = d3d::SECONDARY_GPU;
bool available = d3d::is_multi_gpu_available();
void* native = d3d::get_device(secondary);
```

For DX12, the native pointer is an `ID3D12Device*`. The original zero-argument `get_device()` still returns the primary device. Secondary access returns null when disabled, unavailable or failed; it never aliases GPU 0. Unknown IDs and access outside driver lifetime return null. `has_secondary_gpu()` also reflects secondary availability.

Pointers are borrowed. Use these APIs on the render thread, do not release the returned pointers, and release consumer-owned queues, channels and resources before driver shutdown or recovery. The secondary device is recreated after successful primary recovery. Techniques must recreate their own objects too.

## Capability reporting

Initialization logs adapter names/LUIDs, the HRESULT from `D3D12_FEATURE_D3D12_OPTIONS`, `CrossAdapterRowMajorTextureSupported` and `CrossNodeSharingTier` for each device. Failed queries retain their HRESULT and clear capability fields.

Neither a false row-major flag nor a zero cross-node tier alone excludes a separate adapter pair. The transport uses shared buffers and placed copy footprints and must validate its own resources, formats and fences. Cross-node sharing describes nodes within one adapter. See [D3D12_OPTIONS](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options).

## Health and fallback

Check availability before secondary submission and retain a primary implementation of the technique:

```cpp
if (!d3d::is_multi_gpu_available())
{
    const char* reason = d3d::get_multi_gpu_fallback_reason();
    // Select the technique's primary-GPU path.
}
```

Reasons include disabled configuration, no suitable adapter, enumeration/creation/capability-query failure, device removal, and transfer failure or unsupported operation. Before initialization and after shutdown the reason is `driver not initialized`. The driver logs transitions with `DX12: MGPU fallback to GPU 0:` rather than logging the same reason each frame.

`update_screen()` probes secondary health with `GetDeviceRemovedReason()`. A secondary failure does not initiate primary recovery. The COM object remains owned until teardown/recovery, but subsequent secondary access is disabled.

Report a real failed operation with:

```cpp
d3d::report_secondary_gpu_failure(static_cast<uint32_t>(hr));
```

Successful HRESULTs are ignored. A failure is latched until driver recovery or restart. A busy command/transfer slot is not a device failure. Availability does not guarantee that a particular channel or format will work: check creation and submission results. The driver cannot reroute commands already submitted by a technique.

## Resources and transport

The public transfer API is `nau/3d/dag_drv3d_multi_gpu.h`. It supports tracked primary textures/buffers and native secondary endpoints. A channel owns fixed shared slots, COPY queues and cross-adapter fences. Frame submission uses GPU waits for resource safety; channel destruction drains work on the CPU. `is_transfer_ready(channel, ticket)` polls source-copy completion without recording commands. A pipelined technique receives ready tickets from earlier frames and keeps its previous result while a ticket is pending.

Secondary technique resources use `NauDeviceResourceAllocator` and the existing Nau heap pools. Each technique owns its secondary command submission, states and descriptor objects. GPU 0 and GPU 1 are separate devices, not linked-adapter nodes; their local resource node masks are 1.

See the [technique guide](mgpu_technique_guide.md) for synchronization and lifetime rules, the [fullscreen example](mgpu_fullscreen_sample.md) for output-only transfer, and [SSAO](ambient_occlusion.md) for scene inputs and profiling.

## Functional testing

`dx12.secondaryWarpForTesting=true` explicitly requests a distinct WARP device while `multiGpu` is enabled. It is disabled by default and is labelled TEST by the examples. Normal device selection continues to reject software adapters. WARP exercises separate D3D12 devices and transport but cannot establish hardware performance or physical-removal recovery.

Native tests build independently of the engine with MSVC and the Windows SDK:

```powershell
cmake -S engine/core/modules/render/tests/test_multi_gpu -B build/multi_gpu_tests -A x64
cmake --build build/multi_gpu_tests --config Debug
ctest --test-dir build/multi_gpu_tests -C Debug --output-on-failure
```

Build all default targets so `test_multi_gpu_status` is built along with `test_multi_gpu`. Tests cover selection, COM ownership, fallback persistence, shared fences, transport, native passes, busy slots and retirement. Hardware-pair/debug-capability cases skip if their prerequisites are absent; inspect skip output rather than treating it as hardware coverage.

For integration checks, run the sample with MGPU disabled, with a missing secondary adapter, with WARP, and with an eligible hardware pair when available. Check the actual path, rendered output and fallback logs. Physical removal and device recovery require separate testing.
