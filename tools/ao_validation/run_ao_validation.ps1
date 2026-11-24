param(
    [string]$Root = $PSScriptRoot,
    [ValidateSet('Tests', 'Demo')][string]$Mode = 'Tests',
    [int]$WarmupSeconds = 10,
    [int]$MeasureSeconds = 20
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
$exe = Join-Path $Root 'bin/SceneBaseSample.exe'
$configPath = Join-Path $Root 'samples/sceneBase/config/app.json'
if (!(Test-Path -LiteralPath $exe)) { throw 'Extract the whole package before running this script.' }
$lockPath = Join-Path $Root '.ao-validation.lock'
$lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
$originalConfig = [IO.File]::ReadAllText($configPath)
$utf8 = New-Object System.Text.UTF8Encoding($false)
$results = Join-Path $Root ('results/shared-ao-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $results, (Join-Path $Root 'bin/cache'), (Join-Path $Root 'cache') | Out-Null
$summary = [Collections.Generic.List[object]]::new()
function Save-Json($value, [string]$path) {
    [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 32), $utf8)
}
function Run-Capture([string]$name, [int]$algorithm, [bool]$secondary, [bool]$lifecycle = $false) {
    $folder = Join-Path $results $name
    New-Item -ItemType Directory -Force $folder | Out-Null
    $config = $originalConfig | ConvertFrom-Json
    $config.sample | Add-Member -Force NoteProperty automation ([pscustomobject]@{
        enabled = ($Mode -eq 'Tests'); width = 1920; height = 1080; windowMode = 'auto'
        warmupSeconds = $WarmupSeconds; settleSeconds = 1
        measureSeconds = $(if ($lifecycle) { 31 } else { $MeasureSeconds })
        cameraMotion = $true; lifecycleChecks = $lifecycle
    })
    $config.render.ssao = [pscustomobject]@{ enabled = $true; useSecondary = $secondary; algorithm = $algorithm; mode = 1 }
    $config.render | Add-Member -Force NoteProperty measurements ([pscustomobject]@{enabled = $true; csvPath = (Join-Path $folder 'frames.csv')})
    Save-Json $config $configPath
    Save-Json $config (Join-Path $folder 'settings.json')
    Write-Host "Running $name. Please leave the computer idle."
    $process = Start-Process -FilePath $exe -WorkingDirectory $Root -PassThru -RedirectStandardOutput (Join-Path $folder 'stdout.log') -RedirectStandardError (Join-Path $folder 'stderr.log')
    # Keep the native handle open so Windows PowerShell can read the exit code after the child exits.
    $null = $process.Handle
    if ($Mode -eq 'Demo') { $process.WaitForExit(); return }
    $timeoutMs = 1000 * ($WarmupSeconds + $MeasureSeconds + 240)
    if (!$process.WaitForExit($timeoutMs)) {
        Stop-Process -Id $process.Id
        $summary.Add([pscustomobject]@{run = $name; passed = $false; reason = 'Timed out'; frames = 0})
        return
    }
    $exitCode = $process.ExitCode
    $process.Dispose()
    $log = Get-Content -Raw (Join-Path $folder 'stdout.log')
    $csv = Join-Path $folder 'frames.csv'
    $rows = @()
    if (Test-Path -LiteralPath $csv) { $rows = @(Import-Csv $csv | Where-Object phase -eq '2') }
    $reasons = [Collections.Generic.List[string]]::new()
    if ($null -eq $exitCode) { $reasons.Add('Could not read the process exit code') }
    elseif ($exitCode -ne 0) { $reasons.Add("Exit code $exitCode") }
    if ($rows.Count -eq 0 -or $log -notmatch 'AUTORUN: phase=3') { $reasons.Add('Capture did not finish') }
    if ($log -match 'drain failed|Secondary transfer failure|AUTORUN_WINDOW_ERROR') { $reasons.Add('See GPU/window error in stdout.log') }
    if (@($rows | Where-Object { [long]$_.mailbox_bad_ids -gt 0 }).Count) { $reasons.Add('Mailbox returned mismatched frame IDs') }
    if (!$lifecycle -and $rows.Count) {
        $bad = @($rows | Where-Object { $_.width -ne '1920' -or $_.height -ne '1080' -or $_.ao_width -ne '960' -or $_.ao_height -ne '540' -or $_.ao_valid -ne '1' -or $_.actual_gpu -ne ([int]$secondary).ToString() })
        if ($bad.Count) { $reasons.Add("$($bad.Count) frames had a wrong size, invalid AO or GPU fallback") }
        if ($secondary -and [long]$rows[-1].gpu1_completed -le 0) { $reasons.Add('GPU1 completed no AO jobs') }
    }
    if ($lifecycle -and [regex]::Matches($log, 'AO_LIFECYCLE: step=').Count -lt 10) { $reasons.Add('Not all lifecycle steps completed') }
    if ($lifecycle -and !@($rows | Where-Object actual_gpu -eq '1').Count) { $reasons.Add('Lifecycle test never used GPU1') }
    if ($secondary -and $log -notmatch 'common MGPU mailbox') { $reasons.Add('Common mailbox was not used') }
    $mean = if ($rows.Count) { ($rows | Measure-Object frame_ms -Average).Average } else { 0 }
    $entry = [pscustomobject]@{
        run = $name; passed = ($reasons.Count -eq 0); reason = ($reasons -join '; '); frames = $rows.Count
        mean_frame_ms = $mean; fps_from_mean_frame = $(if ($mean -gt 0) { 1000 / $mean } else { 0 })
        gpu1_completed = $(if ($rows.Count) { $rows[-1].gpu1_completed } else { 0 })
        adapters = ([regex]::Matches($log, 'DX12: GPU [01]: [^\r\n]+LUID[^\r\n]*') | ForEach-Object Value) -join ' | '
    }
    $summary.Add($entry)
    Save-Json $summary.ToArray() (Join-Path $results 'SUMMARY.json')
    Write-Host "$name : passed=$($entry.passed) $($entry.reason)"
}
try {
    foreach ($name in @('NAU_MGPU_TEST_WARP', 'NAU_BENCHMARK_GPU0_LUID', 'NAU_BENCHMARK_GPU1_LUID')) {
        Remove-Item -LiteralPath "Env:/$name" -ErrorAction SilentlyContinue
    }
    Save-Json ([pscustomobject]@{
        started = (Get-Date -Format o); computer = $env:COMPUTERNAME
        gpus = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, PNPDeviceID)
        executable_sha256 = (Get-FileHash -Algorithm SHA256 $exe).Hash
        power_scheme = (powercfg /getactivescheme | Out-String).Trim()
        protocol = 'Three AO algorithms, single GPU and common MGPU/mailbox; half-resolution AO; no synthetic CPU load.'
    }) (Join-Path $results 'MACHINE.json')
    if ($Mode -eq 'Demo') { Run-Capture 'demo' 0 $true; return }
    $selfTest = Join-Path $Root 'bin/MgpuMailboxTest.exe'
    $test = Start-Process -FilePath $selfTest -WorkingDirectory $Root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results 'mailbox-self-test.log') -RedirectStandardError (Join-Path $results 'mailbox-self-test-errors.log')
    $null = $test.Handle
    if (!$test.WaitForExit(90000)) { Stop-Process -Id $test.Id; throw 'Mailbox self-test timed out.' }
    $exitCode = $test.ExitCode
    $test.Dispose()
    if ($null -eq $exitCode) { throw 'Could not read the mailbox self-test exit code. Send the results folder.' }
    if ($exitCode -ne 0) { throw "Mailbox self-test failed (exit $exitCode). Send the results folder." }
    foreach ($algorithm in @(@{name='ssao';id=0}, @{name='hbao';id=2}, @{name='xegtao';id=3})) {
        Run-Capture ($algorithm.name + '-single') $algorithm.id $false
        Run-Capture ($algorithm.name + '-mailbox') $algorithm.id $true
    }
    Run-Capture 'lifecycle' 0 $true $true
    $summary | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $results 'SUMMARY.csv')
    if (@($summary | Where-Object { !$_.passed }).Count) { throw 'Some checks failed. Send the whole results folder, including logs.' }
    Write-Host 'All checks passed.'
}
catch {
    $_ | Out-String | Set-Content -Encoding UTF8 (Join-Path $results 'ERROR.txt')
    throw
}
finally {
    [IO.File]::WriteAllText($configPath, $originalConfig, $utf8)
    Write-Host "Send this whole folder: $results"
    $lock.Dispose()
}
