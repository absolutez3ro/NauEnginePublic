# Secondary-GPU fullscreen sample

SceneBaseSample can render an animated checkerboard with a moving white stripe on GPU 1 and display the transferred texture on GPU 0. The same shader and animation parameters can run on GPU 0 for comparison. F6 switches the requested path; the title reports the actual path and labels WARP as TEST.

The output is a fixed 960×540 texture, scaled to the window. `mgpu_demo_present` runs after the scene and UI and covers them. The scene still renders behind it, so this demonstration is a functional transport example rather than a performance benchmark. For scene-dependent processing, see [SSAO](ambient_occlusion.md).

## Execution

Graphics obtains VS/PS bytecode from the shader cache and creates a `d3d::mgpu::FullscreenProducer`. The producer keeps its native pass and transfer channel inside Render. Its placed resources use `NauDeviceResourceAllocator`; the transport channel owns the cross-adapter shared heap.

1. The GPU 1 DIRECT queue draws a fullscreen triangle and signals readiness.
2. `send_from_secondary` copies the result into a shared slot after that signal.
3. The pass retains `sourceReleased` and orders its next write after the copy has stopped reading the source.
4. `receive_on_primary` copies into a tracked engine texture and orders subsequent GPU 0 commands after receipt.
5. `stretch_rect` presents the texture in the backbuffer.

The primary path draws the same shader through an engine material. Neither normal frame path reads pixels back to the CPU.

## Run

Merge these settings into the existing sample configuration:

```json
"dx12": {"multiGpu": true, "secondaryWarpForTesting": true},
"sample": {"mgpuDemo": {"enabled": true, "useSecondary": true}}
```

This explicitly uses WARP as GPU 1. For a hardware pair set `secondaryWarpForTesting=false`. Software adapters are excluded from normal selection, and GPU 1 never aliases the primary adapter. Set `multiGpu=false` to exercise primary fallback. All new features are disabled in the committed default configuration.

After building SceneBaseSample, run one scenario at a time:

```powershell
./tools/scripts/run_mg12_demo.ps1 -Mode warp
./tools/scripts/run_mg12_demo.ps1 -Mode primary
./tools/scripts/run_mg12_demo.ps1 -Mode disabled
./tools/scripts/run_mg12_demo.ps1 -Mode switch
./tools/scripts/run_mg12_demo.ps1 -Mode failure
python tools/scripts/verify_mg12_capture.py build/mg12-runtime/warp.ppm
```

The script defaults to `build/engine/bin/Release/SceneBaseSample.exe` and a 45-second run. Override `-BuildDirectory`, `-Configuration` or `-Seconds` as needed. It temporarily edits and restores `app.json`, requests normal window closure and saves logs/results under `build/mg12-runtime`. Do not run these scripts concurrently.

`switch` changes the mode every 90 frames through the same control path as F6. `failure` reports an artificial transfer error at frame 120 and verifies primary fallback. This does not simulate physical adapter removal. `no-secondary` verifies missing-device fallback only on a machine with one eligible hardware adapter.

Optional settings `autoSwitchFrames`, `failAtFrame`, `captureAtFrame` and `capturePath` support diagnostics and default to zero/disabled. A capture reads the actual GPU 0 texture once and saves its animation phase. `verify_mg12_capture.py` compares all 518,400 pixels with the shader formula. The runner checks counters and captures as well as process exit status.

## Failure and lifetime

A missing secondary device uses GPU 0 immediately. Real initialization/submission errors go through `report_secondary_gpu_failure`. Busy slots preserve the latest valid image; before the first received image, GPU 0 draws the animation. Five seconds without a successful transfer triggers primary fallback.

Switching to GPU 0 does not destroy the producer mid-frame. Destroy the producer before driver shutdown. Its destructor drains the channel before resetting the native pass; unfinished native work may be retired with its resources and fences. Graphics calls `collectFullscreenResources()` every frame, including primary-only frames, and before driver teardown.

Channel destruction can block on CPU waits. An external fence that never signals can prevent completion; a timeout must not free resources still used by a GPU. Device recovery requires releasing and recreating producers. Artificial failure injection does not prove recovery from physical device removal.

The fullscreen sample does not attach a profiler to its secondary DIRECT/COPY work. Profiler self-tests and a CSV mode label do not measure that path. The [AO implementation](ambient_occlusion.md) demonstrates actual secondary and transfer instrumentation.

For native component tests and API details, see [DX12 MGPU](multi_gpu_dx12.md) and the [integration guide](mgpu_technique_guide.md).
