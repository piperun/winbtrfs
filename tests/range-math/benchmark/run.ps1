# Run the frozen range benchmark in a fresh Windows result root.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('calibration', 'comparison')][string]$Phase,
    [Parameter(Mandatory = $true)][string]$SessionId,
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$ExpectedSha256,
    [Parameter(Mandatory = $true)][ValidateRange(0, 0)][int]$GuestCpu,
    [string]$CalibrationSessionId,
    [ValidateRange(1, 5400)][int]$DeadlineSeconds = 5400,
    [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
$orderNames = @('c-first', 'reference-first', 'rust-first')
$permutations = @('CAB', 'CBA', 'ABC', 'BAC', 'ACB', 'BCA')

function Write-Manifest {
    param([hashtable]$Manifest, [string]$Path)
    $json = ConvertTo-Json -InputObject $Manifest -Depth 12
    $temporary = "$Path.tmp"
    [System.IO.File]::WriteAllText($temporary, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Get-ExeHash {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Wait-OutputTasks {
    param([System.Diagnostics.Process]$Process,
          [System.Threading.Tasks.Task]$StdoutTask,
          [System.Threading.Tasks.Task]$StderrTask)
    if (-not $StdoutTask -or -not $StderrTask) { return }
    $both = [System.Threading.Tasks.Task]::WhenAll(
        [System.Threading.Tasks.Task[]]@($StdoutTask, $StderrTask))
    if (-not $both.Wait(5000)) {
        $Process.StandardOutput.Close()
        $Process.StandardError.Close()
        if (-not $both.Wait(2000)) { throw 'Output capture did not drain within 7 seconds' }
    }
    if ($both.IsFaulted) { throw "Output capture failed: $($both.Exception)" }
}

# GetSystemTimes reports aggregate idle, kernel (including idle) and user ticks.
# GlobalMemoryStatusEx reports actual available physical memory in bytes.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class RangeBenchCounters {
    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime { public uint Low; public uint High; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatus {
        public uint Length, Load;
        public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile;
        public ulong TotalVirtual, AvailVirtual, AvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetSystemTimes(out FileTime idle, out FileTime kernel,
                                               out FileTime user);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);

    private static ulong Ticks(FileTime value) {
        return ((ulong)value.High << 32) | value.Low;
    }

    public static ulong[] Read() {
        FileTime idle, kernel, user;
        if (!GetSystemTimes(out idle, out kernel, out user)) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        MemoryStatus memory = new MemoryStatus();
        memory.Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
        if (!GlobalMemoryStatusEx(ref memory)) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        return new ulong[] { Ticks(idle), Ticks(kernel), Ticks(user), memory.AvailPhys };
    }
}
'@

if ([string]::IsNullOrWhiteSpace($SessionId)) { throw 'SessionId is required' }
if ($ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'ExpectedSha256 must be lowercase SHA-256 hex' }
if ($Phase -eq 'comparison' -and [string]::IsNullOrWhiteSpace($CalibrationSessionId)) {
    throw 'Comparison requires CalibrationSessionId'
}
if (Test-Path -LiteralPath $Root) { throw "Fresh result root required: $Root" }
$exePath = (Resolve-Path -LiteralPath $Exe).ProviderPath
New-Item -ItemType Directory -Path $Root -ErrorAction Stop | Out-Null
$rootPath = (Resolve-Path -LiteralPath $Root).ProviderPath
$manifestPath = Join-Path $rootPath 'manifest.json'
$manifest = @{
    schema = 1; phase = $Phase; session_id = $SessionId; expected_sha256 = $ExpectedSha256;
    guest_cpu = $GuestCpu; smoke = [bool]$Smoke; status = 'running';
    preflight_file = 'preflight.json'; runs = @()
}
if ($Phase -eq 'comparison') { $manifest.calibration_session_id = $CalibrationSessionId }
Write-Manifest $manifest $manifestPath

$runner = [System.Diagnostics.Process]::GetCurrentProcess()
$runner.Refresh()
$incomingMask = $runner.ProcessorAffinity.ToInt64()
$manifest.runner_affinity = @{
    incoming_mask = $incomingMask; steady_mask = $null; restored_mask = $null
}
$exitCode = 0
$phaseClock = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $runner.ProcessorAffinity = [IntPtr]::new(2)
    $runner.Refresh()
    $manifest.runner_affinity.steady_mask = $runner.ProcessorAffinity.ToInt64()
    if ($manifest.runner_affinity.steady_mask -ne 2) {
        throw 'Runner could not stay on guest CPU1'
    }
    Write-Manifest $manifest $manifestPath
    $preflight = @{schema = 1; phase = $Phase; session_id = $SessionId; samples = @()}
    $idleClock = [System.Diagnostics.Stopwatch]::StartNew()
    for ($window = 0; $window -le 4; $window++) {
        if ($window -gt 0) {
            if ($phaseClock.Elapsed.TotalSeconds + 15 -ge $DeadlineSeconds) {
                throw 'Guest phase deadline cannot fit 15-second preflight interval'
            }
            Start-Sleep -Seconds 15
        }
        if ($phaseClock.Elapsed.TotalSeconds -ge $DeadlineSeconds) {
            throw 'Guest phase deadline exceeded during preflight'
        }
        $raw = [RangeBenchCounters]::Read()
        $preflight.samples += @{
            seconds = $idleClock.Elapsed.TotalSeconds;
            idle = $raw[0]; kernel = $raw[1]; user = $raw[2];
            available_bytes = $raw[3]
        }
        Write-Manifest $preflight (Join-Path $rootPath 'preflight.json')
    }
    if (-not $Smoke) {
        for ($window = 1; $window -le 4; $window++) {
            $earlier = $preflight.samples[$window - 1]
            $later = $preflight.samples[$window]
            if ($later.idle -lt $earlier.idle -or
                $later.kernel -lt $earlier.kernel -or
                $later.user -lt $earlier.user) {
                throw "Guest CPU counters reset or invalid in 15-second interval $window"
            }
            $idleDelta = $later.idle - $earlier.idle
            $kernelDelta = $later.kernel - $earlier.kernel
            $userDelta = $later.user - $earlier.user
            $totalDelta = $kernelDelta + $userDelta
            if ($totalDelta -le 0 -or $idleDelta -gt $kernelDelta) {
                throw "Guest CPU counters reset or invalid in 15-second interval $window"
            }
        }
        foreach ($sample in $preflight.samples) {
            if ($sample.available_bytes -lt 1GB) { throw 'Guest available memory below 1 GiB' }
        }
    }
    if ($Smoke) {
        $orders = @('c-first')
    } elseif ($Phase -eq 'calibration') {
        $orders = $orderNames
    } else {
        $orders = @()
        foreach ($permutation in $permutations) {
            foreach ($letter in $permutation.ToCharArray()) {
                $orderIndex = [int][char]$letter - [int][char]'A'
                $orders += $orderNames[$orderIndex]
            }
        }
    }
    for ($index = 0; $index -lt $orders.Count; $index++) {
        if ($phaseClock.Elapsed.TotalSeconds -ge $DeadlineSeconds) {
            throw 'Guest phase deadline exceeded before next run'
        }
        $name = 'run-{0:D2}' -f ($index + 1)
        $order = $orders[$index]
        $runRoot = Join-Path $rootPath $name
        New-Item -ItemType Directory -Path $runRoot -ErrorAction Stop | Out-Null
        $before = Get-ExeHash $exePath
        $entry = @{
            name = $name; order = $order; status = 'running'; exit_code = $null;
            process_id = $null;
            sha256_before = $before; sha256_after = $null;
            started_monotonic = $phaseClock.Elapsed.TotalSeconds;
            ended_monotonic = $null; guest_affinity = @()
        }
        if ($Phase -eq 'comparison' -and -not $Smoke) {
            $entry.block = [int][math]::Floor($index / 3) + 1
            $entry.slot = ($index % 3) + 1
            $entry.permutation = $permutations[$entry.block - 1]
        }
        $manifest.runs += $entry
        Write-Manifest $manifest $manifestPath
        if ($before -cne $ExpectedSha256) { throw "Executable hash mismatch before $name" }

        $info = [System.Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $exePath
        $info.Arguments = if ($Smoke) { '--test' } else { '--bench' }
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.EnvironmentVariables['CRITERION_HOME'] = $runRoot
        $info.EnvironmentVariables['BTRFS_RANGE_ORDER'] = $order
        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $info
        $started = $false
        $stdout = $null
        $stderr = $null
        $stdoutTask = $null
        $stderrTask = $null
        $runClock = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $stdout = [System.IO.File]::Create((Join-Path $runRoot 'stdout.log'))
            $stderr = [System.IO.File]::Create((Join-Path $runRoot 'stderr.log'))
            $runner.Refresh()
            $parentOriginalMask = $runner.ProcessorAffinity.ToInt64()
            if ($parentOriginalMask -ne 2) { throw 'Runner affinity changed before launch' }
            $runner.ProcessorAffinity = [IntPtr]::new(1)
            try {
                $runner.Refresh()
                $verifiedMask = $runner.ProcessorAffinity.ToInt64()
                $entry.launch_affinity = @{
                    parent_process_id = $runner.Id;
                    parent_original_mask = $parentOriginalMask;
                    mask = $verifiedMask;
                    verified_seconds = $runClock.Elapsed.TotalSeconds;
                    start_seconds = $null; return_seconds = $null;
                    parent_restored_mask = $null
                }
                if ($verifiedMask -ne 1) { throw 'Runner could not pin child launch to CPU0' }
                $entry.started_monotonic = $phaseClock.Elapsed.TotalSeconds
                $entry.launch_affinity.start_seconds = $runClock.Elapsed.TotalSeconds
                if (-not $process.Start()) { throw "Could not start $name" }
                $started = $true
                $entry.launch_affinity.return_seconds = $runClock.Elapsed.TotalSeconds
            } finally {
                $runner.ProcessorAffinity = [IntPtr]::new($parentOriginalMask)
                $runner.Refresh()
                if ($entry.launch_affinity) {
                    $entry.launch_affinity.parent_restored_mask = $runner.ProcessorAffinity.ToInt64()
                }
            }
            if ($entry.launch_affinity.parent_restored_mask -ne $parentOriginalMask) {
                throw 'Runner CPU1 affinity was not restored after launch'
            }
            $entry.process_id = $process.Id
            $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
            $stderrTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
            $process.Refresh()
            if (-not $process.HasExited) {
                try {
                    $entry.guest_affinity += @{ seconds = $runClock.Elapsed.TotalSeconds;
                                                mask = $process.ProcessorAffinity.ToInt64() }
                } catch {
                    if (-not $process.HasExited) { throw }
                }
            }
            if ($entry.guest_affinity.Count -gt 0 -and
                $entry.guest_affinity[-1].mask -ne 1) {
                throw "Guest affinity changed for $name"
            }
            Write-Manifest $manifest $manifestPath
            while ($true) {
                $remainingMs = [int][math]::Floor(($DeadlineSeconds -
                    $phaseClock.Elapsed.TotalSeconds) * 1000)
                if ($remainingMs -le 0) { throw 'Guest phase deadline exceeded' }
                if ($process.WaitForExit([math]::Min(1000, $remainingMs))) { break }
                $process.Refresh()
                try {
                    $entry.guest_affinity += @{ seconds = $runClock.Elapsed.TotalSeconds;
                                                mask = $process.ProcessorAffinity.ToInt64() }
                } catch {
                    if (-not $process.HasExited) { throw }
                }
                Write-Manifest $manifest $manifestPath
                if ($entry.guest_affinity.Count -gt 0 -and
                    $entry.guest_affinity[-1].mask -ne 1) {
                    throw "Guest affinity changed for $name"
                }
            }
            Wait-OutputTasks $process $stdoutTask $stderrTask
            if (-not $Smoke -and $entry.guest_affinity.Count -lt 2) {
                throw "Timed run lacks ongoing child affinity samples: $name"
            }
            $entry.exit_code = $process.ExitCode
            $entry.sha256_after = Get-ExeHash $exePath
            $entry.ended_monotonic = $phaseClock.Elapsed.TotalSeconds
            $entry.status = if ($entry.exit_code -eq 0 -and
                               $entry.sha256_after -ceq $ExpectedSha256) { 'complete' } else { 'failed' }
            Write-Manifest $manifest $manifestPath
            if ($entry.exit_code -ne 0) { throw "$name exited $($entry.exit_code)" }
            if ($entry.sha256_after -cne $ExpectedSha256) { throw "Executable hash changed after $name" }
        } catch {
            $entry.status = 'failed'
            $entry.error = $_.Exception.Message
            if ($started) {
                if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $entry.exit_code = $process.ExitCode
            }
            try { Wait-OutputTasks $process $stdoutTask $stderrTask }
            catch { $entry.output_error = $_.Exception.Message }
            $entry.ended_monotonic = $phaseClock.Elapsed.TotalSeconds
            try { $entry.sha256_after = Get-ExeHash $exePath } catch { }
            Write-Manifest $manifest $manifestPath
            throw
        } finally {
            if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            if ($stdout) { $stdout.Dispose() }
            if ($stderr) { $stderr.Dispose() }
            if ($process) { $process.Dispose() }
        }
    }
    $manifest.status = 'complete'
    Write-Manifest $manifest $manifestPath
} catch {
    $manifest.status = 'failed'
    $manifest.error = $_.Exception.Message
    [Console]::Error.WriteLine($_.Exception.Message)
    $exitCode = 2
} finally {
    try {
        $runner.ProcessorAffinity = [IntPtr]::new($incomingMask)
        $runner.Refresh()
        $manifest.runner_affinity.restored_mask = $runner.ProcessorAffinity.ToInt64()
        if ($manifest.runner_affinity.restored_mask -ne $incomingMask) {
            throw 'Runner incoming affinity was not restored'
        }
    } catch {
        $manifest.status = 'failed'
        $manifest.error = "Affinity restoration failed: $($_.Exception.Message)"
        [Console]::Error.WriteLine($manifest.error)
        $exitCode = 2
    }
    Write-Manifest $manifest $manifestPath
    $runner.Dispose()
}
exit $exitCode
