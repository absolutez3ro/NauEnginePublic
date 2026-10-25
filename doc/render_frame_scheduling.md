# CPU/GPU frame scheduling

Graphics previously called `d3d::finish_render_commands()` immediately before and after `executeRenderJobs()` on every frame. In the DX12 backend this flushes pending commands, signals progress and waits for that progress on the CPU. It serializes CPU preparation with GPU execution even when no pre-render jobs require a global drain.

Normal rendering now executes pre-render jobs without these two global drains. The pre-render executor still serializes scene mutations on the render thread and awaited jobs still complete before scene rendering. DX12 resource barriers, upload/renaming rules, deferred destruction and bounded frame-slot reuse remain in effect. Explicit waits for readback, resize, driver teardown and local MGPU copies are unchanged. This does not make every engine operation asynchronous or guarantee freedom from resource lifetime bugs.

For diagnosis or comparison, set this startup configuration:

```json
"graphics": {"serializeRenderJobs": true}
```

The default is `false`. `Graphics: serializeRenderJobs=...` in the log records the selected mode. The option applies independently of whether MGPU or SSAO is enabled. Code that needs completed GPU data must synchronize the particular resource; it must not depend on an incidental global drain each frame.

To compare the same SceneBaseSample workload, run sequentially with the same resolution, camera, SSAO divisor and presentation settings:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -ResolutionDivisor 1 -Profile -NoVsync -CaptureFrame 0 -Seconds 45 -SerializeRenderJobs -Label serialized
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -ResolutionDivisor 1 -Profile -NoVsync -CaptureFrame 0 -Seconds 45 -Label overlap
```

Exclude loading/warmup, repeat runs and report wall-clock frame cadence alongside GPU timestamps. Removing CPU waits need not reduce GPU pass duration, and total frames divided by script duration includes startup/loading. Projected FPS improvements are estimates until measured on the target hardware. A PIX/GPUView trace can show CPU/GPU overlap directly; no such trace is included here.

See [SSAO measurements](ambient_occlusion_measurements.md) for recorded checks and [SSAO](ambient_occlusion.md) for the technique's independent input/output synchronization.

The supplied [RTX 3050 Laptop + Intel Graphics report](measurements/ssao_rtx3050_intel_2026_10_25.md) includes primary serialization on/off. The corresponding GPU 0 Frame means are 1.8780 and 1.8801 ms after excluding 500 frames. These GPU intervals alone do not measure CPU preparation overlap or establish an FPS improvement.
