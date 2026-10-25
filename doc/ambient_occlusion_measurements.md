# SSAO measurement methodology

Use GPU timestamps to compare primary and secondary execution of the same scene. Keep resolution, camera, AO parameters, build configuration and asset-loading conditions comparable. Disable captures and reference rendering during timing runs.

## Record and summarize

After building SceneBaseSample, run from the repository root, sequentially:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -Profile -NoVsync -CaptureFrame 0 -Seconds 45 -Label comparison
./tools/scripts/run_ssao_demo.ps1 -Device secondary -Mode lit -Profile -NoVsync -CaptureFrame 0 -Seconds 45 -Label comparison
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
| `Frame`, GPU 0 | Primary frame GPU interval; pipelined SSAO includes local copies but does not wait for unfinished GPU 1 AO |
| `ssao_reconstruct_normals` | Derive geometric normals from prepass depth into local R16G16_FLOAT on the AO device |
| `ssao_raw`, `ssao_blur_h`, `ssao_blur_v` | Three AO passes on the device doing the computation |
| `ssao_reuse` | Primary work used to show cached AO when secondary submission is busy |
| `/src` | Local resource → shared slot on the sending COPY queue |
| `/dst` | Shared slot → local resource on the receiving COPY queue |
| `/sum` | Sum of the two copy durations, excluding time between them |
| `/span` | Calibrated interval from source copy start to destination copy end, including intermediate waits/scheduling |

Each COPY queue provides its own frequency. Calibration maps both clocks to QPC for `/span`; raw timestamps from different devices are never directly subtracted. `/span` excludes waiting before the first source timestamp. Unsupported timers/calibration produce missing measurements and warnings, not zero durations. Do not mechanically sum overlapping queues into a frame duration.

The profiler mode label comes from startup configuration; F6/F8 do not change it. Use separate runs to compare devices. Presentation defaults to vsync. `-NoVsync` sets `video.vsync=false` and `video.adaptive_vsync=false`; check `DX12: presentation` in the log and the recorded `vsync` result field. Frame counts divided by script duration are not a valid speedup measurement because they include startup/loading and presentation effects. GPU pass durations alone also do not establish useful concurrency.

The shutdown counters satisfy `secondary + primary + reused = frames`; `busy` records secondary pressure. Record them alongside `submitted`, `maxAge`, CSVs, adapter names, actual resolution and exit status. `secondary` counts newly published maps, `reused` counts frames displaying an older published map, and `submitted` counts secondary jobs. A slow secondary should produce reused frames while GPU 0 keeps rendering; a large `maxAge` is expected with WARP. Capture logs must satisfy display frame > source frame. A requested secondary run may fall back, so verify actual work rather than relying on configuration or CSV labels. `compare_gpu_timings.py --real-2gpu` is a report label, not proof that two hardware adapters ran the work.

## Immediate refill in ssao_receive

After consuming the pending output, the late node now immediately tries to queue the current retained prepass depth. The same submission/capture helper serves both graph nodes. It clears the old ticket first, keeps one outstanding job and refuses a second submission for the same source frame. Busy native slots/channels do not introduce waits. This removes the scheduling gap caused by waiting for the next frame's early node; it does not shorten the technique's GPU work or provide temporal reprojection.

A traced RTX 4060 Ti + WARP run submitted 71 jobs, received 70 and refilled from `ssao_receive` after all 70 receptions. One job remained pending at shutdown. The source-frame capture (source 540, display 575) passed the original strict comparison: maximum filtered error 0.00196183, residual mean beyond half an R8 step 6.62e-11. A normal-size resize/switch run checked 110 submissions, 108 receptions/refills, one pending job retired on resize and one pending at shutdown. Its resized capture (source 977, display 1039) passed the strict 0.01 bound with maximum error 0.00887114. Trace checks found no duplicate source-frame submission or second outstanding job. They also rejected a deliberately duplicated submission in a negative check.

Busy injection preserved the published map exactly (source 447, display 500), with 56 receptions/refills. Failure injection received one map, queued one immediate refill, then switched to 1,663 primary frames and exited normally. Primary AO and its preserved depth snapshot passed capture verification. Release static and DLL sample builds succeeded.

A separate small-window/switch run checked 1,500 submissions and 1,498 immediate refills but exceeded the 15-second exit deadline after logging `Graphics shutdown completed`; it is not counted as a normal-exit success. Its shutdown delay was not diagnosed. The subsequent normal-size resize/switch run exited with code zero.

A final 30-second timing run at 634x611 display / 317x305 AO used Release, radius 0.5, blur on, vsync off, captures and submission tracing off, with no concurrent build. It produced 5,139 frames: 111 newly received maps, 3 primary results and 5,025 reused maps; 112 jobs were submitted, 111 from the receive node. Max history age was 110 frames. Pair validation found 223 complete COPY transfers (112 depth, 111 AO). After excluding 500 profiler frames, GPU 0 Frame had 4,639 samples: mean 1.0564 ms, median 0.7363 ms, P95 2.1192 ms. Artifacts use `refill` and `refill-summary` under `build/ssao-runtime`.

These are WARP functional/instrumentation checks. They do not establish an Intel hardware speedup or a 2-3-frame history age. Visible ghosting remains possible: the resized capture displayed source 977 in frame 1039, despite immediate refilling. Faster submission alone cannot eliminate stale AO when computation/transport is slow. The previous reports below describe earlier scheduling versions and are not controlled performance baselines for this change.

## Half-resolution AO and late reception

The current graph uses `z-prepass -> ssao_submit -> fill_gbuffer -> CSM -> ssao_receive -> gbuffer_resolve`. GPU 0 snapshots prepass depth locally, reduces it, submits R32F input and continues scene work. Reception polls before lighting; unfinished work reuses history. The full-size depth snapshot is needed because G-buffer may subsequently write depth. A depth-aware upsample publishes full-size R8 AO. At 1080p the default divisor 2 transports 2,592,000 logical bytes per submitted job, excluding alignment/copy overhead. Divisor 1 preserves full-resolution work for comparison.

Graphics no longer performs two unconditional CPU/GPU drains per frame; `graphics.serializeRenderJobs=true` restores the old behavior for diagnosis. See [frame scheduling](render_frame_scheduling.md). Two initial 20-second full-resolution primary runs completed normally in both modes (3,809 frames serialized, 5,002 without serialization). Those totals include loading; they are not steady-state FPS or a measured speedup. GPU timestamp medians were 0.7670 and 0.7383 ms for primary Frame. No CPU/GPU trace was captured.

Shader checks passed 19/19, including foreground-preserving reduction at odd image edges, identity upsampling at full resolution, rejection across depth discontinuities and neutral background. Final-source pixel captures at 317x305 AO / 634x611 display passed the strict 0.01 filtered bound: source frame 544 was displayed in 549, maximum error 0.00608599, residual mean beyond half an R8 step 6.62e-7. A primary capture confirmed its prepass depth snapshot was preserved exactly until upsampling; the full-size debug presentation matched the resolved map. Release SceneBaseSample built with both static libraries and DLLs.

Two final 30-second runs used RTX 4060 Ti, primary or WARP secondary, `lit`, divisor 2, radius 0.5, blur on, vsync off, captures off. No build ran concurrently; statistics exclude the first 500 profiler frames, while scene loading remained asynchronous.

| Run | Frames | New secondary maps | Primary | Reused | Submitted | Max age, frames | Exit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU 0 | 8018 | 0 | 8018 | 0 | 0 | 0 | 0 |
| GPU 0 + WARP | 4715 | 104 | 2 | 4609 | 105 | 108 | 0 |

| GPU 0 Frame | Samples | Mean, ms | Median, ms | P95, ms |
| --- | ---: | ---: | ---: | ---: |
| Primary | 7518 | 0.8990 | 0.7496 | 1.2646 |
| WARP secondary | 4216 | 2.0902 | 0.7854 | 2.9013 |

The CSV pair check validated 209 complete transfers: 105 depth and 104 AO results. No normal texture is transferred. Primary snapshot/reduction/upsampling and all four secondary passes are recorded. Generated artifacts use `half-snapshot` and `half-snapshot-summary` under `build/ssao-runtime`. These WARP results validate behavior/instrumentation and do not establish a hardware-pair speedup. Do not compare them directly with older reports as a controlled benchmark.

Forced busy preserved the low-resolution published map exactly (source 424, display 500); injected failure published one secondary map and then rendered 1,663 primary frames, with normal exit. A run with SSAO disabled also exited normally. Interactive checks exercised F7 map/off/lit, F8 primary/secondary, title cadence and odd-height resize. The title is applied by the owning window thread at about one-second intervals.

**Temporal quality limitation:** the black trail observed behind moving objects is stale AO history, not evidence of a blur pass running on the wrong device. WARP may take hundreds of milliseconds per job and the current policy has no reprojection, motion rejection or age limit. Source-frame pixel comparison can pass while the displayed moving scene has visible ghosting. Half resolution shortens work but does not solve temporal quality. This remains unresolved and must not be reported as an artifact-free result. Evaluate it separately on the target hardware; use the primary path to distinguish it from spatial AO/blur errors.

## Historical depth-only prepass validation

The following section describes commit `72174c63`, before half-resolution work and late reception. Its one-frame minimum latency is historical and is not a guarantee of the current late poll.

The secondary job now starts between `z-prepass` and `fill_gbuffer`, transfers only DEPTH32, reconstructs local geometric normals, executes SSAO/blur and returns R8 AO. Logical transport is 5 bytes/pixel per submitted job; normals are not sent. The consumer polls an older output before submitting current depth, so the displayed secondary map is at least one frame old. Receiving a ready output still requires a local primary copy.

Targeted checks passed 16 shader tests and 7 native SSAO tests. They cover inclined surfaces including long view distances, depth discontinuities, background/isolated pixels, camera rotation and GPU 1 reconstruction with deliberately invalid input normals. Release SceneBaseSample built with static libraries and DLLs. A window check exercised F7 map/off/lit, F8 primary/secondary, positive reciprocal FPS/ms values, resize from 634×611 to 784×561, and normal exit.

Injected failure published one secondary map, then rendered 2,308 frames on GPU 0 and exited normally. Forced busy reused the published map exactly: source frame 226 was displayed in frame 500. The cached AO can disagree with current geometry/background until a newer map arrives; no reprojection is implemented.

The resized source-frame capture was submitted in frame 544 and displayed in frame 547. Maximum filtered error against that source frame's GPU 0 reference was 0.00376731; mean excess beyond half an R8 step was 6.97e-8, with no pixels beyond the original 0.01 bound. Other captures with independent normal reconstruction had sparse outliers beyond that strict bound. Use the explicit `--independent-normals` verification option and retain its JSON statistics, as described in [SSAO functional checks](ambient_occlusion.md#build-and-functional-checks). The default strict comparison is unchanged.

At 634×611, a separate final-source capture used source frame 502, displayed in frame 503. Maximum filtered error was 0.01386410, with two pixels out of 387,374 beyond 0.01; residual mean after half an R8 step was 1.50e-7 and P99.9 was zero. This passes the explicit independent-normal comparison and fails the default strict maximum-pixel check. Keep that distinction when reporting results.

Two sequential 30-second timing runs used Release, `lit`, radius 0.5, blur enabled, `-Profile -NoVsync -CaptureFrame 0`, with no concurrent build. Statistics exclude the first 500 profiler frames; loading remains asynchronous.

| Run | Frames | New secondary maps | Primary | Reused | Submitted jobs | Maximum history age | Exit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU 0 | 7674 | 0 | 7674 | 0 | 0 | 0 | 0 |
| GPU 0 + WARP, depth-only | 4002 | 32 | 2 | 3968 | 33 | 335 | 0 |

| GPU 0 Frame | Samples | Mean, ms | Median, ms | P95, ms |
| --- | ---: | ---: | ---: | ---: |
| Primary AO | 7174 | 0.9453 | 0.7946 | 1.3210 |
| Pipelined WARP AO | 3501 | 1.3179 | 0.7875 | 2.2968 |

The CSV confirms `z-prepass → ssao → fill_gbuffer → CSM → gbuffer_resolve`, all four secondary passes including `ssao_reconstruct_normals`, and no `ssao_normals/*` transfers. Pair validation found 65 complete COPY pairs: 33 depth and 32 returned AO maps. The final job was not consumed before shutdown. Raw artifacts use the `depth-only` label under `build/ssao-runtime`; `depth-only-summary.md/json` contain the complete statistics.

These are RTX 4060 Ti + software WARP correctness checks, not a hardware-pair speedup or proof of useful queue overlap. FPS in the window title is wall-clock frame cadence; it does not replace GPU timestamps or a queue trace.

## Pipelined check before depth-only transport

This check used commit `e4198077` with the former G-buffer normals channel and submission after G-buffer. It verifies the asynchronous scheduling change. The current depth-only path starts after prepass and additionally reconstructs normals on GPU 1; its measurements must be recorded separately.

On Windows, Release, RTX 4060 Ti + WARP, 634×611, two sequential 30-second runs used `lit`, radius 0.5, blur enabled, `-Profile -NoVsync -CaptureFrame 0`. No build ran concurrently. The first 500 profiler frames were excluded from timing statistics; asset loading remained asynchronous. These are functional/instrumentation results, not a controlled hardware benchmark.

| Run | Frames | New secondary maps | Primary | Reused | Submitted jobs | Maximum history age | Exit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU 0 | 6789 | 0 | 6789 | 0 | 0 | 0 | 0 |
| GPU 0 + WARP, pipelined | 3980 | 31 | 20 | 3929 | 32 | 301 | 0 |

| GPU 0 Frame | Samples | Mean, ms | Median, ms | P95, ms |
| --- | ---: | ---: | ---: | ---: |
| Primary AO | 6289 | 1.1164 | 0.9810 | 1.4612 |
| Pipelined WARP AO | 3479 | 1.1882 | 0.8561 | 2.1460 |

GPU 0 continued rendering while WARP computed each map. The COPY check validated 95 complete pairs: 32 normals, 32 depth, 31 returned AO maps. The final submitted map was not consumed before shutdown. COPY timing halves are retained by ticket and published together, so a long secondary job is not discarded by the generic profiler's primary-frame age limit. No missing-half warnings occurred in this run.

A separate capture compared the output from source frame 602 against the primary reference of frame 602, displayed it in frame 734, and exited normally. Maximum error was 0.00196558237; mean excess beyond half an R8 quantization step was 2.35e-11. The age of 132 frames reflects slow software WARP; a hardware secondary is expected to have different latency. The implementation guarantees a minimum age of one frame and reuses history rather than imposing a fixed maximum delay. Motion artifacts and CPU contention from WARP remain relevant.

Targeted native transfer/SSAO checks passed 22/22, including a source held behind an unsignaled fence while the consumer continued independent work, and readiness of overwritten tickets in a three-slot ring. Release SceneBaseSample built with both static libraries and DLLs. Additional runs verified injected failure (1 secondary map, then primary fallback), forced busy (the displayed map exactly matched history), device switching, and resize from 634×611 to 784×561. The resized capture matched its source-frame reference within the documented tolerance. These runs all exited with code 0.

Generated artifacts: `build/ssao-runtime/pipeline-summary.{md,json}`, `*-profile-pipeline-final-timings.csv`, `warp-lit-pipeline-capture-verification.json`. They are local build outputs. Use the commands above with a hardware secondary and record actual adapter names for a hardware report. A PIX/GPUView trace is still needed to inspect hardware queue overlap directly.

## Historical example: synchronous functional check with WARP

These numbers predate the pipelined scheduling change and include the former same-frame wait. They are retained as an instrumentation example, not as measurements of the new path.

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
