# SSAO measurement methodology

Use GPU timestamps to compare primary and secondary execution of the same scene. Keep resolution, camera, AO parameters, build configuration and asset-loading conditions comparable. Disable captures and reference rendering during timing runs.

## Record and summarize

After building SceneBaseSample, run from the repository root, sequentially:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -Profile -CaptureFrame 0 -Seconds 45 -Label comparison
./tools/scripts/run_ssao_demo.ps1 -Device secondary -Mode lit -Profile -CaptureFrame 0 -Seconds 45 -Label comparison
python tools/scripts/summarize_ssao_timings.py `
    build/ssao-runtime/lit-profile-comparison-timings.csv `
    build/ssao-runtime/secondary-lit-profile-comparison-timings.csv `
    --warmup-frames 10 --output build/ssao-runtime/comparison
```

Use `-Device warp` instead of `secondary` for a functional test; its output prefix is `warp-lit`. The runner defaults to `build/engine`, Release, and restores the original configuration after exit. Do not run instances concurrently. For direct executable launches, set `$env:NAU_GPU_PROFILER='1'` in the launching PowerShell session. The `GPU profiler enabled` log line contains the exact CSV path. If opening it fails, check the logged error and parent directory.

The summary validates finite, nonnegative timings, duplicate rows, transfer direction and `/sum = /src + /dst` within CSV rounding tolerance. It writes Markdown and JSON containing sample counts, mean, median and P95. Warmup exclusion affects statistics; pair integrity is checked across the complete CSV. Async scene loading and a short warmup do not make this a controlled benchmark: repeat runs after loading and report the conditions.

## Interpret the measurements

| Name or suffix | Meaning |
| --- | --- |
| `Frame`, GPU 0 | Primary frame GPU interval, including waits for secondary AO |
| `ssao_pack_normals` | Extract sphere-map XY into R16G16_FLOAT |
| `ssao_raw`, `ssao_blur_h`, `ssao_blur_v` | Three AO passes on the device doing the computation |
| `ssao_reuse` | Primary work used to show cached AO when secondary submission is busy |
| `/src` | Local resource → shared slot on the sending COPY queue |
| `/dst` | Shared slot → local resource on the receiving COPY queue |
| `/sum` | Sum of the two copy durations, excluding time between them |
| `/span` | Calibrated interval from source copy start to destination copy end, including intermediate waits/scheduling |

Each COPY queue provides its own frequency. Calibration maps both clocks to QPC for `/span`; raw timestamps from different devices are never directly subtracted. `/span` excludes waiting before the first source timestamp. Unsupported timers/calibration produce missing measurements and warnings, not zero durations. Do not mechanically sum overlapping queues into a frame duration.

The profiler mode label comes from startup configuration; F6/F8 do not change it. Use separate runs to compare devices. The driver currently chooses `VSYNCED` presentation. Frame counts divided by script duration are not a valid speedup measurement because they include startup/loading and presentation effects. GPU pass durations alone also do not establish useful concurrency.

The shutdown counters satisfy `secondary + primary + reused = frames`; `busy` records secondary pressure. Record them alongside CSVs, adapter names, actual resolution and exit status. A requested secondary run may fall back, so verify actual secondary work rather than relying on configuration or CSV labels. `compare_gpu_timings.py --real-2gpu` is a report label, not proof that two hardware adapters ran the work.

## Example: functional check with WARP

Reference build: [source revision 63eb0a0d](https://github.com/absolutez3ro/NauEnginePublic/commit/63eb0a0d43624ddf0e607eda2ca54586792dc66d), Windows, Release, RTX 4060 Ti as GPU 0, render size 634×611, initial camera, `lit`, radius 0.5, blur enabled. GPU 1 in the second run is **WARP**, a software adapter. Both runs lasted 45 seconds without captures/reference rendering; the scene loaded asynchronously. These measurements demonstrate timing collection and cannot establish hardware-pair performance.

| Run | SSAO frames | Secondary | Primary | Busy | Reused | Exit code |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Primary | 9359 | 0 | 9359 | 0 | 0 | 0 |
| Primary + WARP | 69 | 69 | 0 | 0 | 0 | 0 |

The first ten profiler frames were excluded from statistics. Pass sample counts can differ because unfinished DIRECT queries at shutdown are not emitted as zeros.

| Run | Pass | GPU | Samples | Mean, ms | Median, ms | P95, ms |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Primary | Frame | 0 | 9364 | 1.1490 | 0.9595 | 1.5268 |
| Primary | ssao_pack_normals | 0 | 9357 | 0.0102 | 0.0082 | 0.0102 |
| Primary | ssao_raw | 0 | 9357 | 0.0767 | 0.0778 | 0.0963 |
| Primary | ssao_blur_h | 0 | 9357 | 0.0574 | 0.0440 | 0.0553 |
| Primary | ssao_blur_v | 0 | 9357 | 0.0559 | 0.0430 | 0.0543 |
| WARP | Frame | 0 | 58 | 652.0625 | 641.8816 | 688.4695 |
| WARP | ssao_pack_normals | 0 | 58 | 0.0870 | 0.0594 | 0.0614 |
| WARP | ssao_raw | 1 | 60 | 133.1487 | 133.1051 | 149.9772 |
| WARP | ssao_blur_h | 1 | 60 | 154.6479 | 155.6228 | 171.9003 |
| WARP | ssao_blur_v | 1 | 60 | 153.3084 | 153.6579 | 173.5221 |

COPY timestamp frequencies were 1,000,000,000 Hz on GPU 0 and 10,000,000 Hz on WARP. The full CSVs contained 207 validated pairs: 69 per channel. Each row below summarizes 60 pairs after warmup.

| Transfer | Part | GPU/direction | Mean, ms | Median, ms | P95, ms |
| --- | --- | --- | ---: | ---: | ---: |
| normals | src | 0 | 0.4072 | 0.4294 | 0.4337 |
| normals | dst | 1 | 94.3425 | 92.7246 | 104.3433 |
| normals | sum | 0→1 | 94.7497 | 93.1576 | 104.7714 |
| normals | span | 0→1 | 94.9093 | 93.3039 | 104.9317 |
| depth | src | 0 | 0.4076 | 0.4301 | 0.4358 |
| depth | dst | 1 | 94.2002 | 92.6597 | 103.8382 |
| depth | sum | 0→1 | 94.6078 | 93.0901 | 104.2702 |
| depth | span | 0→1 | 94.7472 | 93.2357 | 104.4184 |
| AO result | src | 1 | 93.4544 | 92.1518 | 101.8910 |
| AO result | dst | 0 | 0.1171 | 0.1230 | 0.1240 |
| AO result | sum | 1→0 | 93.5714 | 92.2296 | 102.0143 |
| AO result | span | 1→0 | 93.6597 | 92.3184 | 102.1021 |

The implementation's native/shader tests and pixel comparisons validate correctness separately from these timings. Raw CSVs and logs are generated artifacts under `build/ssao-runtime`, not engine source files. For a new hardware report, attach its raw artifacts and conditions to the relevant review; do not reuse the WARP numbers as a hardware result. See [SSAO](ambient_occlusion.md) for functional checks and known limitations.
