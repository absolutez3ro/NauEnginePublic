# Screen-space ambient occlusion

SceneBaseSample provides SSAO on the primary GPU and an optional secondary-GPU path. The secondary path sends depth and normals to GPU 1, computes SSAO and two blur passes, and returns a completed result from an earlier frame before lighting resolve. GPU 0 does not wait for the secondary job submitted in its current frame. Enable it through the existing sample configuration; it is disabled by default.

For integration of other techniques, see the [MGPU technique guide](mgpu_technique_guide.md). The [fullscreen sample](mgpu_fullscreen_sample.md) demonstrates the simpler case with no scene inputs.

## Algorithm and attribution

The reference is the classic SSAO implementation by **Bogdanov M.K.** in [Multi-GPU-Render-DX12 at commit 9146f83f](https://github.com/Mikhanil/Multi-GPU-Render-DX12/tree/9146f83f4251a2b8acd4e9cc361f9367be597a4c):

- [Ssao.hlsl](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/9146f83f4251a2b8acd4e9cc361f9367be597a4c/Shaders/Common/Ssao.hlsl): position reconstruction and 14 occlusion samples.
- [SsaoBlur.hlsl](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/9146f83f4251a2b8acd4e9cc361f9367be597a4c/Shaders/Common/SsaoBlur.hlsl): separable, surface-aware Gaussian filtering.
- [SharedSSAO.cpp](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/9146f83f4251a2b8acd4e9cc361f9367be597a4c/Common/SharedSSAO.cpp): sample directions, resources and passes.

This implements classic SSAO, not the HBAO or XeGTAO algorithms also present in that repository. Nau uses its own transfer channels, resource pools and render graph.

The shader reconstructs a view-space position from depth and converts the G-buffer normal to view space. Fourteen directions cover the eight cube corners and six face directions. A deterministic pixel hash reflects these directions into the normal hemisphere; their lengths span 0.25–1 times the radius. Projected samples estimate occlusion using depth separation, normal alignment, `surfaceEpsilon` and distance fading. Accessibility is `pow(1 - occlusionSum / 14, 6)`.

Two 11-tap Gaussian passes, with sigma 2.5, filter the result horizontally and vertically. Samples across a normal discontinuity (`dot < 0.8`) or the configured view-depth threshold are rejected. AO=1 is neutral. Deferred resolve multiplies ambient diffuse and reflected lighting by screen-space AO in addition to material AO; direct lighting and emission remain independent of this factor.

## Nau conventions and resources

- The camera looks along -Z, projection uses reversed-Z, and shaders use `mul(matrix, vector)`. Position reconstruction uses the G-buffer projection. Background depth=0 produces AO=1.
- G-buffer normals are world-space sphere-map XY values. A packing pass extracts them into `R16G16_FLOAT`; both devices evaluate AO from the same packed normals.
- Samples outside the image or in front of the near plane are rejected. Reconstruction uses the sampled pixel center to avoid false occlusion on inclined planes.
- Raw AO and horizontal blur use `R32F`; the final filtered map uses `R8_UNORM`. All passes run at the render resolution.
- The frame graph owns GPU 0 textures. `NauDeviceResourceAllocator` allocates GPU 1 placed resources from Nau heap pools. Root signatures, PSOs, descriptor heaps and a DIRECT queue belong to GPU 1. Shader bytecode comes from the existing asset pipeline.
- When the subsystem is disabled, resolve still binds an existing white texture to its AO SRV. It creates no full-size AO targets or AO passes. Keep this neutral binding valid even when the shader branch disables AO.

The node runs after `fill_gbuffer` and before `gbuffer_resolve`:

```mermaid
flowchart LR
    G[G-buffer on GPU 0] --> N[Pack normals]
    N --> T[Transfer normals and depth]
    T --> A[GPU 1: SSAO for frame N]
    A --> H[Horizontal blur]
    H --> V[Vertical blur]
    V --> O[Transfer AO to GPU 0]
    O --> C[Cache after source copy completes]
    C --> R[Lighting resolve in a later frame]
```

## Transfer and synchronization

| Data | Direction | Format | Bytes/pixel |
| --- | --- | --- | ---: |
| Sphere-map normals XY | 0 → 1 | R16G16_FLOAT | 4 |
| Raw reversed-Z depth | 0 → 1 | DEPTH32, received as R32_FLOAT | 4 |
| Filtered AO | 1 → 0 | R8_UNORM | 1 |

At 1920×1080 this is 18,662,400 logical bytes per frame. Row alignment, shared slots and the two local copies of each transfer add overhead. Depth is copied, not directly sampled across adapters; linearization happens in the AO shader.

`submitSsao` and `tryReceiveSsao` use three channels from `dag_drv3d_multi_gpu.h`, each with three shared slots. The normal path holds one outstanding secondary job; it does not queue more input frames while that job is pending. Constants, output tickets and the source frame number remain associated until reception.

At the start of the AO node, poll the previous output ticket with `is_transfer_ready`. If the source COPY is still pending, keep the last valid AO map and issue no primary receive/wait for that ticket. Once ready, copy it into the alternate persistent GPU 0 AO texture and swap the two maps. This receive still orders a local GPU 0 copy against previous uses of that destination; it has no unfinished GPU 1 work to wait for. The filtered frame-graph output is filled from the published map.

After polling, send current normals and depth only if the outstanding job was consumed and the pass can submit. Input COPY queues wait for the previous native input use; the DIRECT queue waits for both input-ready points. The output channel waits for the pass and returns a retained ticket. The next overwrite waits for `sourceReleased`. GPU 0 continues resolve/post-processing with an older map while GPU 1 computes the newly submitted job. If GPU 1 takes longer, inputs are skipped until it finishes and history is reused across multiple frames. Startup before the first valid secondary result computes locally; an actual failure also switches to GPU 0.

The minimum latency is **one frame**, increasing when GPU 1 is slower. This scheduling follows the readiness check and available-result reuse in [HybridSSAOApp by Bogdanov M.K.](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/9146f83f4251a2b8acd4e9cc361f9367be597a4c/MGPU-SSAO/HybridSSAOApp.cpp#L215), adapted to Nau's channels and fence ownership. The compatibility `renderSsao` API still schedules an immediate GPU wait; SceneBaseSample uses the split API.

Constants occupy a 224-byte cbuffer with aligned slots. Slots are reused only after their work completes. Frame submission does not wait for completion on the CPU:

- `submitted`: inputs/work were queued by `submitSsao`, or an older result was received by `tryReceiveSsao`.
- `busy`: keep pending work and reuse the last received map. Before a valid map exists, compute AO on GPU 0. An early busy check avoids packing and sending inputs; a later channel refusal can occur after partial submission.
- `failed`: report the secondary failure and use GPU 0.

Resize, mode changes and device changes invalidate the displayed map. Pending results from before a switch are consumed and discarded when secondary execution resumes; they cannot become the new history. History has no reprojection or maximum age; moving geometry or a moving camera can expose stale shading while GPU 1 remains busy.

On resize or shutdown, drain channels before releasing the native pass, its resources and allocator. The pass can retain unfinished work for deferred collection. `collectFullscreenResources()` also collects retired SSAO resources and is called by Graphics each frame and before driver teardown. Channel destruction can block on the CPU; an external fence that never signals can prevent shutdown.

## Configuration and controls

Merge these properties into the existing `samples/sceneBase/config/app.json`; preserve other application settings:

```json
"dx12": {"multiGpu": true, "secondaryWarpForTesting": false},
"sample": {
    "ssao": {"enabled": true, "mode": 1, "radius": 0.5, "blur": true, "useSecondary": true},
    "mgpuDemo": {"enabled": false, "useSecondary": true}
}
```

`mode`: 0 disables the effect, 1 shows the lit scene, 2 shows the AO map. F7 cycles lit → map → off; F8 requests primary/secondary execution. The window title and shutdown counters report the actual path. With SSAO enabled the sample starts its camera at (0, 5, 10), tilted down toward the scene. Move with WASD/QE; rotate by dragging with the left mouse button.

`secondaryWarpForTesting=true` explicitly selects a distinct WARP device, labelled TEST. WARP checks functionality, not hardware performance. A missing or failed secondary device uses GPU 0. Disable the fullscreen demo when viewing AO because it covers the scene.

Defaults are `radius=0.5`, `fadeStart=0.2`, `fadeEnd=1`, `surfaceEpsilon=0.05`, and `blurDepthThreshold=0.2` in scene units. Radius zero produces neutral AO; `blur=false` shows raw AO. These parameters are read at startup.

## Build and functional checks

Configure and build SceneBaseSample with the repository's usual engine build settings. Scripts default to `build/engine/bin/Release/SceneBaseSample.exe`; override `-BuildDirectory` and `-Configuration` for another build. Reconfigure CMake after shader changes: configuration copies shader sources into sample resources.

Run scripts sequentially; each temporarily edits and then restores the same configuration file:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -Interactive -CaptureFrame 0
./tools/scripts/run_ssao_demo.ps1 -Device warp -Mode lit -CaptureFrame 20 -Seconds 45
python tools/scripts/verify_ssao_capture.py build/ssao-runtime/warp-lit --mode lit
./tools/scripts/run_ssao_demo.ps1 -Device busy -Mode lit -CaptureFrame 20 -Seconds 45
python tools/scripts/verify_ssao_capture.py build/ssao-runtime/busy-lit --mode busy
./tools/scripts/run_ssao_demo.ps1 -Device fallback -Mode lit -Seconds 15
./tools/scripts/run_ssao_demo.ps1 -Device device-switch -Mode lit -CaptureFrame 60 -Seconds 45
```

Other diagnostic modes are `ao`, `off`, `raw`, `zero-radius`, `switch`, `disabled` and device `busy-first`. Secondary validation captures source inputs and a GPU 0 reference when a job is submitted, then reads its outputs and presentation when that job is received in a later frame. Logs record display and source frame IDs. CaptureFrame is the earliest requested source frame. Busy injection waits for valid history; failure injection waits for a received secondary result. Extra capture work is absent with `-CaptureFrame 0`. Captures must contain loaded geometry; a very early capture can fail while assets are still loading.

Verification checks finite values, [0,1] bounds, neutral background, geometry, and scene/map consistency. Secondary comparison allows maximum error 0.01 after blur or 0.02 for raw AO. After subtracting half an R8 quantization step (`0.5/255`, clamped at zero), residual mean must be ≤0.00001 and P99.9 ≤0.001. This accommodates quantization and small differences near depth/sample boundaries.

Standalone shader tests execute the production HLSL on D3D11 WARP, including planes, contacts, silhouettes, normal transforms, blur boundaries and an odd image size. Native pass/channel tests use D3D12. Both complement an engine run:

```powershell
cmake -S engine/core/modules/graphics/tests/test_ssao_shader -B build/ssao_tests -A x64
cmake --build build/ssao_tests --config Debug
ctest --test-dir build/ssao_tests -C Debug --output-on-failure
cmake -S engine/core/modules/render/tests/test_multi_gpu -B build/multi_gpu_tests -A x64
cmake --build build/multi_gpu_tests --config Debug
ctest --test-dir build/multi_gpu_tests -C Debug --output-on-failure
```

## Profiling and limitations

Use `-Profile -CaptureFrame 0` for timing runs, or set `NAU_GPU_PROFILER=1` in the process launching the executable. The log's `GPU profiler enabled` line gives the CSV path. See [measurement methodology and WARP example](ambient_occlusion_measurements.md).

The profiler records primary `Frame`, primary AO passes when used, secondary `ssao_raw`, `ssao_blur_h`, `ssao_blur_v`, and COPY halves for `ssao_normals`, `ssao_depth`, `ssao_result`. Each queue has its own timestamp frequency; cross-queue intervals use clock calibration. Unsupported COPY timestamps are omitted with a warning rather than written as zero. The optional transfer observer remains alive until both channel queues drain. Shutdown also reports `submitted` jobs and maximum displayed history age (`maxAge`). `secondary + primary + reused = frames` counts displayed frames; `submitted` counts jobs sent to GPU 1.

The CSV mode label comes from startup configuration; F7/F8 do not update it. Compare separate runs. Primary `Frame` includes local input/output copies, but pipelined SSAO does not insert a wait for an unfinished secondary AO job. Presentation defaults to vsync; set `video.vsync=false` and `video.adaptive_vsync=false`, or use the runner's `-NoVsync` flag. GPU timings alone do not prove useful overlap or a speedup. Hardware-pair performance has not been established by the WARP measurements.

Device recovery requires destroying and recreating technique owners around driver recovery. Channel shutdown retains blocking waits. Rare shutdown failures during asynchronous texture loading and `cache/dx12.cache` errors remain unresolved; this implementation does not claim to fix them.

## Source map

| Component | Source |
| --- | --- |
| Shader inputs, reconstruction, samples and blur | `samples/sceneBase/content/shaders/include/ssao_common.hlsli`, `src/ssao.hlsl`, `src/ssao_blur.hlsl` |
| Frame graph, materials, controls and history | `engine/core/modules/graphics/src/render_window/ambient_occlusion.*`, `ambient_occlusion_nodes.cpp` |
| Public producer API and constants | `engine/core/modules/render/include/nau/3d/multi_gpu_ssao*.h` |
| Native pass, transport bridge and retirement | `engine/core/modules/render/src/drv3d_DX12/device_ssao_pass.*`, `multi_gpu_ssao.cpp` |
| DIRECT/COPY profiler adapters | `engine/core/modules/graphics/src/render_window/ambient_occlusion*profiler*` |
