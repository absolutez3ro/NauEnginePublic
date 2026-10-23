param(
    [ValidateSet('disabled', 'off', 'lit', 'ao', 'raw', 'zero-radius', 'switch')]
    [string]$Mode = 'lit',
    [ValidateRange(5, 600)]
    [int]$Seconds = 20,
    [string]$BuildDirectory = 'build/engine',
    [string]$Configuration = 'Release',
    [switch]$Interactive,
    [ValidateSet('primary', 'secondary', 'warp', 'fallback', 'device-switch')]
    [string]$Device = 'primary',
    [ValidateRange(1, 10000)]
    [int]$CaptureFrame = 150
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$configPath = Join-Path $repo 'samples/sceneBase/config/app.json'
$originalConfig = [IO.File]::ReadAllBytes($configPath)
$binaryDir = Join-Path (Join-Path $repo $BuildDirectory) "bin/$Configuration"
$resultDir = Join-Path $repo 'build/ssao-runtime'
$runName = if ($Device -eq 'primary') { $Mode } else { "$Device-$Mode" }
New-Item -ItemType Directory -Path $resultDir -Force | Out-Null
$environmentNames = @('NAU_GPU_PROFILER', 'NAU_GPU_PROFILER_SELFTEST')
$originalEnvironment = @{}
foreach ($name in $environmentNames) { $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null
try {
    $config = [Text.Encoding]::UTF8.GetString($originalConfig) | ConvertFrom-Json
    $config.dx12.multiGpu = $Device -ne 'primary'
    $config.dx12.secondaryWarpForTesting = $Device -in @('warp', 'fallback', 'device-switch')
    $config.sample.mgpuDemo.enabled = $false
    $config.sample.ssao = [ordered]@{
        enabled = $Mode -ne 'disabled'
        mode = $(if ($Mode -eq 'off') { 0 } elseif ($Mode -in @('ao', 'raw', 'zero-radius')) { 2 } else { 1 })
        radius = $(if ($Mode -eq 'zero-radius') { 0.0 } else { 0.5 })
        blur = $Mode -ne 'raw'
        autoSwitchFrames = $(if ($Mode -eq 'switch') { 60 } else { 0 })
        useSecondary = $Device -ne 'primary'
        validateSecondary = $Device -in @('secondary', 'warp', 'device-switch')
        autoDeviceSwitchFrames = $(if ($Device -eq 'device-switch') { 3 } else { 0 })
        failAtFrame = $(if ($Device -eq 'fallback') { 3 } else { 0 })
        captureAtFrame = $CaptureFrame
        capturePrefix = (Join-Path $resultDir $runName)
    }
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 20))
    $env:NAU_GPU_PROFILER = '0'
    $env:NAU_GPU_PROFILER_SELFTEST = '0'
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
    $result | ConvertTo-Json | Set-Content (Join-Path $resultDir "$runName.result.json")
    $result | ConvertTo-Json
    if ($early -or $forced -or $process.ExitCode -ne 0) { throw "SSAO runtime failed: $stdoutPath" }
    if ($Interactive) {
        if (!$shutdown.Success -and $Mode -ne 'disabled') { throw 'SSAO did not start before the window closed' }
    } elseif ($Mode -eq 'disabled') {
        if ($shutdown.Success) { throw 'SSAO executed while disabled' }
    } else {
        if (!$shutdown.Success) { throw 'SSAO did not shut down normally' }
        $suffixes = @('raw.f32', 'filtered.f32', 'normals.f32', 'depth.f32', 'scene.ppm')
        if ($Device -in @('warp', 'secondary', 'device-switch') -and $Mode -ne 'off') { $suffixes += 'reference.f32' }
        foreach ($suffix in $suffixes) {
            $capture = Get-Item -LiteralPath (Join-Path $resultDir "$runName-$suffix") -ErrorAction SilentlyContinue
            if (!$capture -or $capture.LastWriteTime -lt $started) { throw "Missing fresh capture: $runName-$suffix" }
        }
        if ($Mode -eq 'switch') {
            foreach ($state in 0..2) { if ($log -notmatch "SSAO: mode=$state,") { throw "Mode $state was not exercised" } }
        }
        if ($Device -in @('warp', 'secondary', 'device-switch') -and $Mode -ne 'off') {
            if ($result.secondary -eq 0) { throw 'No frame was computed on GPU 1' }
            if ($log -notmatch "SSAO: secondary capture frame=$CaptureFrame") { throw 'The capture did not come from GPU 1' }
        }
        if ($Device -eq 'fallback') {
            if ($result.secondary -eq 0 -or $result.primary -eq 0 -or $log -notmatch 'injecting secondary failure') { throw 'Fallback was not exercised' }
        }
        if ($Device -eq 'device-switch' -and $result.primary -eq 0) { throw 'GPU 0 was not exercised' }
    }
} finally {
    if ($process -and !$process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(15000)) { $process.Kill() }
    }
    [IO.File]::WriteAllBytes($configPath, $originalConfig)
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process') }
}
