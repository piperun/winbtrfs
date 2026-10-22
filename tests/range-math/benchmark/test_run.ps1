# Windows integration regression: inherited child mask, output, exit, restoration.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Runner,
    [Parameter(Mandatory = $true)][string]$Root
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Root) { throw "Fresh probe root required: $Root" }
$runnerPath = (Resolve-Path -LiteralPath $Runner).ProviderPath
New-Item -ItemType Directory -Path $Root | Out-Null
$rootPath = (Resolve-Path -LiteralPath $Root).ProviderPath
$probeExe = Join-Path $rootPath 'probe-child.exe'

Add-Type -OutputAssembly $probeExe -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class ProbeChild {
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessAffinityMask(IntPtr process,
        out IntPtr processMask, out IntPtr systemMask);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    public static int Main(string[] args) {
        IntPtr processMask, systemMask;
        if (!GetProcessAffinityMask(GetCurrentProcess(), out processMask, out systemMask)) {
            return 91;
        }
        Console.WriteLine("initial_mask=" + processMask.ToInt64());
        Console.Error.WriteLine("probe-stderr");
        if (Environment.GetEnvironmentVariable("RANGE_PROBE_MODE") == "fail") {
            return 7;
        }
        Thread.Sleep(30000);
        return 0;
    }
}
'@

$hash = (Get-FileHash -LiteralPath $probeExe -Algorithm SHA256).Hash.ToLowerInvariant()

function Assert-Result {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Stop-OwnedProcess {
    param([System.Diagnostics.Process]$Process)
    if (-not $Process) { return }
    try {
        $Process.Refresh()
        if (-not $Process.HasExited) { $Process.Kill() }
        if (-not $Process.WaitForExit(5000)) {
            throw "Owned process $($Process.Id) did not exit within 5 seconds"
        }
    } finally {
        $Process.Dispose()
    }
}

function Stop-ProbeChildren {
    foreach ($probeProcess in [System.Diagnostics.Process]::GetProcessesByName('probe-child')) {
        try {
            $probeProcess.Refresh()
            if ($probeProcess.HasExited) { continue }
            try { $actualPath = $probeProcess.MainModule.FileName }
            catch {
                $probeProcess.Refresh()
                if ($probeProcess.HasExited) { continue }
                throw
            }
            if (-not [string]::Equals($actualPath, $probeExe,
                                     [System.StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $probeProcess.Kill()
            if (-not $probeProcess.WaitForExit(5000)) {
                throw "Probe child $($probeProcess.Id) did not exit within 5 seconds"
            }
        } finally {
            $probeProcess.Dispose()
        }
    }
}

function Runner-Arguments {
    param([string]$Session, [string]$Destination)
    $format = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" ' +
              '-Phase calibration -SessionId {1} -Root "{2}" -Exe "{3}" ' +
              '-ExpectedSha256 {4} -GuestCpu 0 -DeadlineSeconds 240 -Smoke'
    return ($format -f $runnerPath, $Session, $Destination, $probeExe, $hash)
}

$originalMode = $env:RANGE_PROBE_MODE
$failure = $null
$changeProcess = $null
$failureRoot = Join-Path $rootPath 'nonzero-output'
$changeRoot = Join-Path $rootPath 'affinity-change'
try {
    $env:RANGE_PROBE_MODE = 'fail'
    $failureArgs = Runner-Arguments 'probe-failure' $failureRoot
    $failure = [System.Diagnostics.Process]::Start('powershell.exe', $failureArgs)
    Assert-Result ($failure -is [System.Diagnostics.Process]) 'Failure runner handle was not retained'
    if (-not $failure.WaitForExit(150000)) { throw 'Failure probe timed out' }
    $failureManifest = Get-Content -LiteralPath (Join-Path $failureRoot 'manifest.json') -Raw |
        ConvertFrom-Json
    $failureRun = $failureManifest.runs[0]
    $stdout = Get-Content -LiteralPath (Join-Path $failureRoot 'run-01/stdout.log') -Raw
    $stderr = Get-Content -LiteralPath (Join-Path $failureRoot 'run-01/stderr.log') -Raw
    Assert-Result ($failure.ExitCode -ne 0 -and $failureManifest.status -eq 'failed' -and
                   $failureRun.exit_code -eq 7) 'Nonzero child exit was not preserved'
    Assert-Result -Condition ($stdout -match 'initial_mask=1' -and
                             $stderr -match 'probe-stderr') `
                  -Message 'Inherited startup mask or async output capture failed'
    Assert-Result ($failureRun.launch_affinity.mask -eq 1 -and
                   $failureRun.launch_affinity.parent_original_mask -eq 2 -and
                   $failureRun.launch_affinity.parent_restored_mask -eq 2 -and
                   $failureManifest.runner_affinity.restored_mask -eq
                   $failureManifest.runner_affinity.incoming_mask) `
                  -Message 'Parent launch or final affinity restoration failed'

    $env:RANGE_PROBE_MODE = 'wait'
    $changeArgs = Runner-Arguments 'probe-affinity' $changeRoot
    $changeProcess = [System.Diagnostics.Process]::Start('powershell.exe', $changeArgs)
    Assert-Result ($changeProcess -is [System.Diagnostics.Process]) 'Affinity runner handle was not retained'
    $deadline = [DateTime]::UtcNow.AddSeconds(150)
    $changed = $false
    while ([DateTime]::UtcNow -lt $deadline -and -not $changeProcess.HasExited) {
        $manifestPath = Join-Path $changeRoot 'manifest.json'
        if (Test-Path -LiteralPath $manifestPath) {
            try {
                $state = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
                if ($state.runs.Count -gt 0 -and $state.runs[0].process_id) {
                    $child = Get-Process -Id $state.runs[0].process_id -ErrorAction Stop
                    $child.ProcessorAffinity = [IntPtr]::new(2)
                    $child.Refresh()
                    Assert-Result -Condition ($child.ProcessorAffinity.ToInt64() -eq 2) `
                                  -Message 'Could not change probe child affinity'
                    $changed = $true
                    break
                }
            } catch [System.IO.IOException] { }
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-Result $changed 'Did not observe a live child to change its affinity'
    if (-not $changeProcess.WaitForExit(45000)) { throw 'Affinity probe timed out' }
    $changeManifest = Get-Content -LiteralPath (Join-Path $changeRoot 'manifest.json') -Raw |
        ConvertFrom-Json
    Assert-Result ($changeProcess.ExitCode -ne 0 -and $changeManifest.status -eq 'failed' -and
                   $changeManifest.runs[0].error -match 'Guest affinity changed' -and
                   @($changeManifest.runs[0].guest_affinity | Where-Object mask -eq 2).Count -gt 0) `
                  'Runner did not catch the external child affinity change'
    Write-Output "Windows runner probes passed; evidence: $rootPath"
} finally {
    try {
        try { Stop-OwnedProcess $changeProcess }
        finally {
            Stop-OwnedProcess $failure
        }
    } finally {
        try { Stop-ProbeChildren }
        finally { $env:RANGE_PROBE_MODE = $originalMode }
    }
}
