# DX12 secondary GPU discovery

Set `dx12.multiGpu` to `true` in `samples/sceneBase/config/app.json` to create a second native D3D12 device. The default is `false`. Applications with their own Dagor settings provider can also set `multiGpu:b=yes` in the `dx12` DataBlock. An explicitly supplied application property takes precedence over the DataBlock value.

The primary adapter selection and render pipeline stay unchanged. With the flag disabled there is no additional adapter enumeration, device creation, capability probing, or multi-GPU logging. With the flag enabled, secondary selection reuses the normal sorted candidate list, excluding the primary LUID and software/WARP adapters. Explicit primary selection by LUID, monitor, or WARP uses a separate enumeration for the secondary candidates. Driver preferences and the configured D3D feature level still apply. A failed secondary creation tries the next candidate; exhausting the list logs a warning and continues rendering on GPU 0.

## Native access for MG-04 consumers

The declarations are in `nau/3d/dag_drv3d.h`:

```cpp
d3d::GpuId primary = d3d::PRIMARY_GPU;     // 0
d3d::GpuId secondary = d3d::SECONDARY_GPU; // 1
bool available = d3d::has_secondary_gpu();
void* native = d3d::get_device(secondary);
```

For DX12, cast the returned pointer to `ID3D12Device*`. GPU 0 refers to the existing primary device; the original zero-argument `d3d::get_device()` remains available. GPU 1 returns `nullptr` when disabled or unavailable. Unknown ids and calls outside an initialized driver's lifetime also return `nullptr`. Consumers can select GPU 0 explicitly for fallback or treat a null result as a refusal; GPU 1 never silently aliases GPU 0.

These are borrowed pointers. Use them on the render thread, do not release them, and release consumer-owned resources before driver shutdown or device recovery. The secondary device is released before DXGI/debug/runtime teardown and recreated after successful primary recovery. This task creates no secondary queues, shared resources, transfers, fences, or double buffering; those remain for the later MG implementations.

## Capability log

With `multiGpu` enabled, `NAU_LOG_INFO` reports each device's name and LUID, the HRESULT from `D3D12_FEATURE_D3D12_OPTIONS`, `CrossAdapterRowMajorTextureSupported`, and `CrossNodeSharingTier`. Failed queries retain the HRESULT and clear the capability fields.

These values are reported independently of whether a second device exists. A false row-major flag permits cross-adapter texture copies but restricts direct view usage. Cross-node sharing describes nodes within one adapter and is not a prerequisite for two separate adapters. See [Microsoft's D3D12_OPTIONS documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options).

## Verification

The native component tests can run independently of an engine build, using MSVC and the Windows SDK:

```powershell
cmake -S engine/core/modules/render/tests/test_multi_gpu -B build/multi_gpu_tests
cmake --build build/multi_gpu_tests --config Debug
ctest --test-dir build/multi_gpu_tests -C Debug --output-on-failure
```

They check distinct LUIDs, software/WARP rejection, description and creation failures, empty results, COM ownership, reset, destruction, and capability queries using a real WARP device. The separate physical-device test skips when two D3D12 hardware adapters are unavailable. It never counts WARP as a second physical GPU.

For acceptance on a two-GPU machine:

1. Run SceneBaseSample with the flag absent or `false`; compare its log and rendered scene with the baseline.
2. Set the flag to `true`; verify GPU 0 and GPU 1 names and distinct LUIDs, plus both capability query results, while the scene continues rendering.
3. Check `get_device(0) == get_device()`, `has_secondary_gpu()`, a non-null `get_device(1)`, and a null result for an unknown id.
4. Close the sample with DX12 CPU validation enabled and check for secondary-device leaks; also test device recovery if the test setup supports it.
5. Run with only one hardware GPU; check the warning, `has_secondary_gpu() == false`, and continued primary rendering.
