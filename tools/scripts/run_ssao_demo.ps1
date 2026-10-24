param(
    [ValidateSet('disabled', 'off', 'lit', 'ao', 'raw', 'zero-radius', 'switch')]
    [string]$Mode = 'lit',
    [ValidateRange(5, 600)]
    [int]$Seconds = 20,
    [string]$BuildDirectory = 'build/engine',
    [string]$Configuration = 'Release',
    [switch]$Interactive,
    [ValidateSet('primary', 'secondary', 'warp', 'fallback', 'device-switch', 'busy', 'busy-first')]
    [string]$Device = 'primary',
    [ValidateRange(0, 10000)]
    [int]$CaptureFrame = 150,
    [switch]$Profile,
    [switch]$NoVsync,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')]
    [string]$Label
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$configPath = Join-Path $repo 'samples/sceneBase/config/app.json'
$originalConfig = [IO.File]::ReadAllBytes($configPath)
$binaryDir = Join-Path (Join-Path $repo $BuildDirectory) "bin/$Configuration"
$resultDir = Join-Path $repo 'build/ssao-runtime'
$runName = if ($Device -eq 'primary') { $Mode } else { "$Device-$Mode" }
if ($Profile) { $runName += '-profile' }
if ($Label) { $runName += "-$Label" }
New-Item -ItemType Directory -Path $resultDir -Force | Out-Null
$environmentNames = @('NAU_GPU_PROFILER', 'NAU_GPU_PROFILER_SELFTEST', 'NAU_GPU_PROFILER_CSV', 'NAU_GPU_PROFILER_LOG_EVERY')
$originalEnvironment = @{}
foreach ($name in $environmentNames) { $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null
try {
    $config = [Text.Encoding]::UTF8.GetString($originalConfig) | ConvertFrom-Json
    $config.dx12.multiGpu = $Device -ne 'primary'
    $config.dx12.secondaryWarpForTesting = $Device -in @('warp', 'fallback', 'device-switch', 'busy', 'busy-first')
    if (!$config.video) { $config | Add-Member -NotePropertyName video -NotePropertyValue ([pscustomobject]@{}) }
    $config.video | Add-Member -NotePropertyName vsync -NotePropertyValue (!$NoVsync) -Force
    $config.video | Add-Member -NotePropertyName adaptive_vsync -NotePropertyValue $false -Force
    $config.sample.mgpuDemo.enabled = $false
    $config.sample.ssao = [ordered]@{
        enabled = $Mode -ne 'disabled'
        mode = $(if ($Mode -eq 'off') { 0 } elseif ($Mode -in @('ao', 'raw', 'zero-radius')) { 2 } else { 1 })
        radius = $(if ($Mode -eq 'zero-radius') { 0.0 } else { 0.5 })
        blur = $Mode -ne 'raw'
        autoSwitchFrames = $(if ($Mode -eq 'switch') { 60 } else { 0 })
        useSecondary = $Device -ne 'primary'
        validateSecondary = !$Profile -and $Device -in @('secondary', 'warp', 'device-switch', 'busy-first')
        autoDeviceSwitchFrames = $(if ($Device -eq 'device-switch') { 1000 } else { 0 })
        failAtFrame = $(if ($Device -eq 'fallback') { 3 } else { 0 })
        busyAtFrame = $(if ($Device -eq 'busy') { $CaptureFrame } elseif ($Device -eq 'busy-first') { 1 } else { 0 })
        captureAtFrame = $CaptureFrame
        capturePrefix = (Join-Path $resultDir $runName)
    }
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 20))
    $env:NAU_GPU_PROFILER = $(if ($Profile) { '1' } else { '0' })
    $env:NAU_GPU_PROFILER_SELFTEST = '0'
    $env:NAU_GPU_PROFILER_CSV = Join-Path $resultDir "$runName-timings.csv"
    $env:NAU_GPU_PROFILER_LOG_EVERY = '0'
    $started = Get-Date
    $stdoutPath = Join-Path $resultDir "$runName.stdout.log"
    $windowStyle = if ($Interactive) { 'Normal' } else { 'Hidden' }
    $process = Start-Process -FilePath (Join-Path $binaryDir 'SceneBaseSample.exe') -WorkingDirectory $binaryDir -WindowStyle $windowStyle -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError (Join-Path $resultDir "$runName.stderr.log")
    $early = $false
    if ($Interactive) { $process.WaitForExit() } else { $early = $process.WaitForExit($Seconds * 1000) }
    $forced = $false
    if (!$Interactive -and !$early) {
        $process.Refresh()
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(15000)) { $forced = $true; $process.Kill(); $process.WaitForExit(5000) | Out-Null }
    }
    $process.Refresh()
    $log = [IO.File]::ReadAllText($stdoutPath)
    $shutdown = [regex]::Match($log, 'SSAO: shutdown, frames=(\d+)')
    $counters = [regex]::Match($log, 'SSAO: shutdown, frames=\d+, secondary=(\d+), primary=(\d+), busy=(\d+)')
    $result = [ordered]@{mode=$Mode; device=$Device; exitCode=$process.ExitCode; exitedEarly=$early; forcedTermination=$forced; frames=$(if ($shutdown.Success) { [long]$shutdown.Groups[1].Value } else { 0 }); secondary=$(if ($counters.Success) { [long]$counters.Groups[1].Value } else { 0 }); primary=$(if ($counters.Success) { [long]$counters.Groups[2].Value } else { 0 })}
    $reused = [regex]::Match($log, 'busy=\d+, reused=(\d+)')
    $result.busy = $(if ($counters.Success) { [long]$counters.Groups[3].Value } else { 0 })
    $result.reused = $(if ($reused.Success) { [long]$reused.Groups[1].Value } else { 0 })
    $result.validateSecondary = $config.sample.ssao.validateSecondary
    $result.profile = [bool]$Profile
    $result.captureFrame = $CaptureFrame
    $result.vsync = !$NoVsync
    $pipeline = [regex]::Match($log, 'reused=\d+, submitted=(\d+), maxAge=(\d+)')
    $result.submitted = $(if ($pipeline.Success) { [long]$pipeline.Groups[1].Value } else { 0 })
    $result.maxHistoryAge = $(if ($pipeline.Success) { [long]$pipeline.Groups[2].Value } else { 0 })
    $secondaryCaptures = [regex]::Matches($log, 'SSAO: secondary capture frame=(\d+), source=(\d+)')
    $secondaryCapture = if ($secondaryCaptures.Count -gt 0) { $secondaryCaptures[$secondaryCaptures.Count - 1] } else { [regex]::Match('', 'capture') }
    $captureEvents = [regex]::Matches($log, 'SSAO: (?:secondary|reused) capture frame=(\d+), source=(\d+)')
    $captureEvent = if ($captureEvents.Count -gt 0) { $captureEvents[$captureEvents.Count - 1] } else { $secondaryCapture }
    $result.captureDisplayFrame = $(if ($captureEvent.Success) { [long]$captureEvent.Groups[1].Value } else { $CaptureFrame })
    $result.captureSourceFrame = $(if ($captureEvent.Success) { [long]$captureEvent.Groups[2].Value } else { $CaptureFrame })
    $result | ConvertTo-Json | Set-Content (Join-Path $resultDir "$runName.result.json")
    $result | ConvertTo-Json
    if ($early -or $forced -or $process.ExitCode -ne 0) { throw "SSAO runtime failed: $stdoutPath" }
    if ($Interactive) {
        if (!$shutdown.Success -and $Mode -ne 'disabled') { throw 'SSAO did not start before the window closed' }
    } elseif ($Mode -eq 'disabled') {
        if ($shutdown.Success) { throw 'SSAO executed while disabled' }
    } else {
        if (!$shutdown.Success) { throw 'SSAO did not shut down normally' }
        if ($result.frames -ne $result.secondary + $result.primary + $result.reused) { throw 'Frame accounting mismatch' }
        $suffixes = @()
        if ($CaptureFrame -gt 0) {
            $suffixes = @('filtered.f32', 'normals.f32', 'depth.f32', 'scene.ppm')
            if ($Device -eq 'busy') { $suffixes += 'history.f32' } else { $suffixes += 'raw.f32' }
            if ($config.sample.ssao.validateSecondary -and $Mode -ne 'off') { $suffixes += 'reference.f32' }
        }
        foreach ($suffix in $suffixes) {
            $capture = Get-Item -LiteralPath (Join-Path $resultDir "$runName-$suffix") -ErrorAction SilentlyContinue
            if (!$capture -or $capture.LastWriteTime -lt $started) { throw "Missing fresh capture: $runName-$suffix" }
        }
        if ($Mode -eq 'switch') {
            foreach ($state in 0..2) { if ($log -notmatch "SSAO: mode=$state,") { throw "Mode $state was not exercised" } }
        }
        if ($Device -in @('warp', 'secondary', 'device-switch', 'busy-first') -and $Mode -ne 'off') {
            if ($result.secondary -eq 0) { throw 'No frame was computed on GPU 1' }
            if ($CaptureFrame -gt 0 -and (!$secondaryCapture.Success -or $result.captureSourceFrame -lt $CaptureFrame -or $result.captureDisplayFrame -le $result.captureSourceFrame)) { throw 'The capture did not come from an older, completed GPU 1 submission' }
        }
        if ($Device -eq 'fallback') {
            if ($result.secondary -eq 0 -or $result.primary -eq 0 -or $log -notmatch 'injecting secondary failure') { throw 'Fallback was not exercised' }
        }
        if ($Device -eq 'device-switch' -and $result.primary -eq 0) { throw 'GPU 0 was not exercised' }
        if ($Device -eq 'busy' -and ($result.reused -eq 0 -or $log -notmatch 'SSAO: reused capture frame=\d+, source=\d+')) { throw 'Cached AO was not used instead of GPU 0 recomputation' }
        if ($Device -eq 'busy-first' -and ($result.primary -lt 1 -or $result.busy -lt 1 -or $log -notmatch 'SSAO: injecting busy at frame 1')) { throw 'Initial busy frame did not fall back without history' }
        if ($Profile) {
            $timingFile = Get-Item -LiteralPath $env:NAU_GPU_PROFILER_CSV
            if ($timingFile.LastWriteTime -lt $started) { throw 'No fresh profiler CSV' }
            $timings = @(Import-Csv -LiteralPath $timingFile.FullName)
            if (!($timings | Where-Object { $_.gpu -eq '0' -and $_.pass -eq 'Frame' })) { throw 'No GPU 0 frame timings' }
            if ($result.secondary -gt 0) {
                foreach ($pass in @('ssao_reconstruct_normals', 'ssao_raw', 'ssao_blur_h', 'ssao_blur_v')) {
                    if (!($timings | Where-Object { $_.gpu -eq '1' -and $_.pass -eq $pass -and [double]$_.ms -gt 0 })) { throw "No GPU 1 timing for $pass" }
                }
                foreach ($transfer in @('ssao_depth', 'ssao_result')) {
                    $bothSupported = $true
                    foreach ($half in @('src', 'dst')) {
                        if ($log.Contains("transfer '$transfer' $half copy timestamps unavailable")) {
                            $bothSupported = $false
                            continue
                        }
                        if (!($timings | Where-Object { $_.pass -eq "$transfer/$half" -and [double]$_.ms -ge 0 })) { throw "No COPY timing for $transfer/$half" }
                    }
                    if ($bothSupported -and !($timings | Where-Object { $_.pass -eq "$transfer/sum" })) { throw "No paired COPY timing for $transfer" }
                }
            }
        }
    }
} finally {
    if ($process -and !$process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(15000)) { $process.Kill() }
    }
    [IO.File]::WriteAllBytes($configPath, $originalConfig)
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process') }
}
