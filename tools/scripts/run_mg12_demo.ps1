param(
    [ValidateSet('disabled', 'primary', 'warp', 'no-secondary', 'switch', 'failure')]
    [string]$Mode = 'warp',
    [ValidateRange(5, 600)]
    [int]$Seconds = 45,
    [string]$BuildDirectory = 'build/engine',
    [string]$Configuration = 'Release'
)

# Run one sample acceptance scenario and restore the user's configuration even on failure.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$configPath = Join-Path $repo 'samples/sceneBase/config/app.json'
$originalConfig = [IO.File]::ReadAllBytes($configPath)
$binaryDir = Join-Path (Join-Path $repo $BuildDirectory) "bin/$Configuration"
$resultDir = Join-Path $repo 'build/mg12-runtime'
$caseName = $Mode
New-Item -ItemType Directory -Path $resultDir -Force | Out-Null
$environmentNames = @('NAU_GPU_PROFILER', 'NAU_GPU_PROFILER_SELFTEST')
$originalEnvironment = @{}
foreach ($name in $environmentNames) { $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null
try {
    $config = [Text.Encoding]::UTF8.GetString($originalConfig) | ConvertFrom-Json
    $config.dx12.multiGpu = $Mode -ne 'disabled'
    $config.dx12.secondaryWarpForTesting = $Mode -in @('primary', 'warp', 'switch', 'failure')
    $config.sample.mgpuDemo = [ordered]@{
        enabled = $true
        useSecondary = $Mode -ne 'primary'
        autoSwitchFrames = $(if ($Mode -eq 'switch') { 90 } else { 0 })
        failAtFrame = $(if ($Mode -eq 'failure') { 120 } else { 0 })
        captureAtFrame = $(if ($Mode -eq 'failure') { 180 } elseif ($Mode -eq 'switch') { 240 } else { 120 })
        capturePath = (Join-Path $resultDir "$caseName.ppm")
    }
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 20))
    $env:NAU_GPU_PROFILER = '0'
    $env:NAU_GPU_PROFILER_SELFTEST = '0'
    $started = Get-Date
    $process = Start-Process -FilePath (Join-Path $binaryDir 'SceneBaseSample.exe') -WorkingDirectory $binaryDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $resultDir "$caseName.stdout.log") -RedirectStandardError (Join-Path $resultDir "$caseName.stderr.log")
    $processId = $process.Id
    $exitedEarly = $process.WaitForExit($Seconds * 1000)
    $closeRequested = $false
    $forcedTermination = $false
    if (!$exitedEarly) {
        $process.Refresh()
        $closeRequested = $process.CloseMainWindow()
        if (!$process.WaitForExit(15000)) {
            $forcedTermination = $true
            $process.Kill()
            $process.WaitForExit(5000) | Out-Null
        }
    }
    $process.Refresh()
    $stdoutPath = Join-Path $resultDir "$caseName.stdout.log"
    $stdoutText = [IO.File]::ReadAllText($stdoutPath)
    $shutdown = [regex]::Match($stdoutText, 'MG-12: shutdown, frames=(\d+), transferred=(\d+), primary=(\d+)')
    $frames = if ($shutdown.Success) { [long]$shutdown.Groups[1].Value } else { 0 }
    $transferred = if ($shutdown.Success) { [long]$shutdown.Groups[2].Value } else { 0 }
    $primary = if ($shutdown.Success) { [long]$shutdown.Groups[3].Value } else { 0 }
    $capturePath = Join-Path $resultDir "$caseName.ppm"
    $capture = Get-Item -LiteralPath $capturePath -ErrorAction SilentlyContinue
    $currentCapture = $capture -and $capture.LastWriteTime -ge $started
    $result = [ordered]@{ mode = $Mode; processId = $processId; seconds = ((Get-Date) - $started).TotalSeconds; exitedEarly = $exitedEarly; closeRequested = $closeRequested; forcedTermination = $forcedTermination; exitCode = $process.ExitCode }
    $result.frames = $frames
    $result.transferred = $transferred
    $result.primary = $primary
    $result.currentCapture = [bool]$currentCapture
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDir "$caseName.result.json")
    $result | ConvertTo-Json
    $logs = Get-ChildItem (Join-Path $binaryDir 'logs') -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started }
    foreach ($log in $logs) { Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $resultDir "$caseName-$($log.Name)") }
    if ($forcedTermination -or $exitedEarly -or $process.ExitCode -ne 0) { throw "MG-12 scenario failed; inspect $resultDir" }
    if (!$shutdown.Success -or !$currentCapture) { throw "MG-12 did not finish the requested capture; increase Seconds or inspect $stdoutPath" }
    if ($Mode -in @('warp', 'switch', 'failure') -and $transferred -eq 0) { throw "No secondary frames transferred; inspect $stdoutPath" }
    if ($Mode -in @('primary', 'disabled', 'switch', 'failure') -and $primary -eq 0) { throw "Single-GPU path did not execute; inspect $stdoutPath" }
    if ($Mode -in @('primary', 'disabled') -and $transferred -ne 0) { throw "Unexpected secondary rendering in $Mode" }
    if ($Mode -eq 'switch' -and $stdoutText -notmatch 'MG-12: mode=GPU 1 WARP') { throw 'Switch never returned to the secondary path' }
    if ($Mode -eq 'failure' -and $stdoutText -notmatch 'MG-12: mode=GPU 0 \| secondary transfer failed') { throw 'Failure did not trigger MG-11 fallback' }
} finally {
    if ($process -and !$process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(15000)) { $process.Kill() }
    }
    [IO.File]::WriteAllBytes($configPath, $originalConfig)
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process') }
}
