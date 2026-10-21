# Windows byte-range helper benchmark

This standalone runner compares the exact production C and Rust clipping
helpers through their C ABI. It measures 1024 calls per Criterion iteration,
including loop, call, output and barrier cost. It does not measure the driver,
filesystem I/O, locking or query traversal. The raw same-signature C baseline
is an overhead reference; do not subtract it from either helper result.

The generator uses SplitMix64 with seed `0x6e5a4b139c20d7f1`. Every dataset
has 1024 pre-generated entries:

| Dataset | Contents |
| --- | --- |
| `overlap` | 1024 extents spanning a 100-byte query, clipped at both ends |
| `before` | 1024 extents ending exactly at the query start; all miss |
| `mixed` | 256 both-end clips, 192 start clips, 192 end clips, 256 contained extents, 128 before-query misses; Fisher-Yates shuffled |
| `u64-stress` | 512 overflowing extent lengths near `u64::MAX`, 192 contained near-limit extents, 64 after-query misses, 128 zero lengths, 128 reversed queries |

Only `u64-stress` contains inputs excluded by ordinary caller validation. The
driver's extent traversal stops at or after the query end, so after-query
misses are stress coverage rather than part of the driver-shaped mixed set.
The independent `u128` oracle and both helper results are checked before any
Criterion registration. A mismatch aborts the executable. The runner writes
`inputs.json` under `CRITERION_HOME`, including registration order, seed and FNV-1a 64-bit
fingerprints over each case's four little-endian input words.

Build from this directory with the cached Rust 1.98 toolchain and Criterion
0.8.2 lockfile. The runner uses Rust `std`; the separately compiled production
Rust leaf uses `no_std` and must have no unresolved symbols. `build.rs` records
the exact C, Rust, `nm` and archive commands in its Cargo `OUT_DIR` as
`build-commands.txt`, and rejects unresolved Rust leaf references. No download
fallback is permitted.

```sh
export CARGO_TARGET_DIR="$(git rev-parse --show-toplevel)/build/range-benchmark/target"
export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=gcc
RUSTFLAGS='-C debuginfo=0' cargo +1.98.0 bench --offline --locked \
  --target x86_64-pc-windows-gnu --no-run
```

The build script chooses `x86_64-w64-mingw32-{gcc,ar,nm,objcopy,objdump}` for Windows and
`{gcc,ar,nm,objcopy,objdump}` for Linux, with matching `BTRFS_RANGE_*` overrides. The C
helper is compiled with `-O2 -fno-lto`; its symbol alone is renamed. The Rust
leaf uses `opt-level=2`, overflow checks, panic abort, one codegen unit, no
red zone, no LTO and no debug info. The benchmark profile has no debug info,
no LTO and one codegen unit. No CPU-specific tuning is used.

To include the original Rust helper in the same executable, rebuild its local
object from the base revision using the same Rust 1.98 target and leaf flags:

```sh
git show da5321e44dbaa9621ba68910c81851680c8a312e:rust/range-math/lib.rs > /path/to/original-range.rs
rustc +1.98.0 --crate-name btrfs_range_math --crate-type rlib --edition=2024 \
  --target x86_64-pc-windows-gnu -C opt-level=2 -C panic=abort \
  -C overflow-checks=on -C codegen-units=1 -C no-redzone=yes \
  -C lto=off -C debuginfo=0 --emit=obj \
  -o /path/to/original-range.o /path/to/original-range.rs
export BTRFS_RANGE_REFERENCE_OBJECT=/path/to/original-range.o
RUSTFLAGS='-C debuginfo=0' cargo +1.98.0 bench --offline --locked \
  --target x86_64-pc-windows-gnu --no-run
```

The reference path must be a trusted local object for the selected target,
with only the original `btrfs_clip_byte_range` export and no unresolved
symbols. The build script copies it into Cargo's output directory and renames
only that export to `btrfs_clip_byte_range_reference`. It rejects a missing
file, wrong architecture, wrong export or unresolved symbols before linking.
Cargo tracks the path, file content and environment variable for rebuilds.
The input object is never modified. Leave the variable unset to retain the
two-helper runner.

Copy the `ranges-*.exe` file printed by the build command to the Windows VM.
Set `CRITERION_HOME` to a dedicated output directory for each run. Leave
`BTRFS_RANGE_ORDER` unset or set it to `c-first` for normal registration order;
set it to `rust-first` for reversed order. With a reference object, run
`c-first`, `reference-first`, then `rust-first`; these cyclic orders put each
helper once in every measured position while keeping the call-only baseline
last. The `--test` smoke mode runs the preflight and Criterion's test mode.
A full run omits `--test`:

```cmd
set CRITERION_HOME=C:\WinBtrfsTests\range-benchmark\smoke
set BTRFS_RANGE_ORDER=c-first
ranges.exe --test

set CRITERION_HOME=C:\WinBtrfsTests\range-benchmark\order-c-first
set BTRFS_RANGE_ORDER=c-first
ranges.exe --bench

set CRITERION_HOME=C:\WinBtrfsTests\range-benchmark\order-rust-first
set BTRFS_RANGE_ORDER=rust-first
ranges.exe --bench
```

With a reference object linked, add the third order:

```cmd
set CRITERION_HOME=C:\WinBtrfsTests\range-benchmark\order-reference-first
set BTRFS_RANGE_ORDER=reference-first
ranges.exe --bench
```

Run each measurement serially without compiling or doing other heavy work
on the host. Criterion uses 100 samples, 2 seconds warmup, 5 seconds measurement
and 95% confidence intervals. Preserve each directory's `new/sample.json`,
`new/estimates.json`, `new/benchmark.json` and `inputs.json`. The result roots
must differ so a run cannot become the other's statistical baseline.

Normalize completed runs with:

```sh
python3 summarize.py PATH_TO_C_FIRST PATH_TO_RUST_FIRST > summary.json
# With a reference object:
python3 summarize.py PATH_TO_C_FIRST PATH_TO_RUST_FIRST PATH_TO_REFERENCE_FIRST > summary.json
```

The summarizer divides Criterion's batch nanoseconds and interval endpoints by
1024, preserves each interval's actual `confidence_level`, prefers its slope
estimate when present, and reports point Rust/C, candidate/original Rust and
original Rust/C ratios where available. It does not infer a confidence
interval for any ratio. Inspect raw samples
and order sensitivity before drawing a performance conclusion. The baseline
is reported as its own raw time per call.

## Controlled three-helper comparison

`run.ps1`, `hostmonitor.py` and `assess.py` run and validate a fixed experiment
with the frozen three-helper executable. Supply its expected SHA-256 explicitly;
for the current executable at `build/range-benchmark/optimization/ranges-three-way.exe`
it is `d384285c86b3172e77ebd28acd7620abe2c160b3d17b3c9899f8f3bb88ded15a`.
The tools do not rebuild or modify the executable. Use the same executable and
the same expected hash in both phases.

Before launching the VM, measure its cgroup memory charges and record the guest
RAM and cgroup limit. With qcow2 drives using QEMU's writeback cache, host
file-cache charges can reach the cgroup limit even when anonymous memory leaves
room. For a controlled comparison, launch the qcow2 block drives with
`cache=none`; this requests direct host I/O while retaining flushes. Verify
each drive in QMP `query-block`: `cache.direct` must be `true` and
`cache.no-flush` must be `false`. Keep the firmware drives and other VM settings
unchanged. Record the actual cgroup `memory.current`, `memory.stat` and
`memory.events.local` before and throughout the experiment; the monitor checks
for new limit or OOM events.

The Windows runner records raw aggregate idle, kernel and user CPU counters
and available physical memory at the start and end of four consecutive
15-second intervals. It rejects invalid or reset counters and any snapshot
with less than 1 GiB available RAM. The assessor recomputes each interval's
busy CPU fraction from the session-linked `preflight.json` and reports it,
without an idle percentage cutoff. Let Windows Update finish naturally. Keep
host build jobs and other
load off the chosen physical core and its SMT siblings. Set the QEMU benchmark
vCPU (`CPU 0/KVM`) to one quiet host CPU and keep all other QEMU threads off
that CPU and its siblings. Record the actual thread masks; the monitor verifies
them throughout each phase. The host monitor confines itself and its SSH child
to CPUs outside that core and its SMT siblings. QMP `query-cpus-fast` supplies
the actual vCPU thread IDs; thread names do not identify vCPU roles. The
benchmark guest must fit its cgroup without
new `memory.events.local` max, oom, or kill events.

The PowerShell runner holds its own process on guest CPU1 while collecting
preflight data, writing logs and monitoring each child. Immediately before
`Process.Start`, it temporarily sets and verifies its own mask to guest CPU0.
Windows children inherit that process mask, so the benchmark starts on CPU0
even if `Process.Start` takes time to return. The runner restores CPU1 as soon
as the start call returns, records the return time, then reads the child's
actual mask throughout each timed run. It restores the incoming runner mask
when the phase ends or fails. A fast smoke child may finish before its first
direct mask read; smoke is never timing evidence.

Use a new session ID and fresh Windows result root for calibration. Run the
host monitor from Linux around the SSH command that invokes PowerShell. The
command after `--` is passed as arguments, without a shell. Keep the host
output directory distinct from the Windows result root. Example with simple
paths and shell variables already set to the actual machine values:

```sh
python3 hostmonitor.py --output-dir "$HOST_CAL" --pid "$QEMU_PID" \
  --qmp-socket "$QMP_SOCKET" \
  --expected-cpu "$HOST_CPU" --session-id "$CAL_ID" --phase calibration -- \
  ssh "$WINDOWS_HOST" powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\bench\run.ps1' -Phase calibration -SessionId "$CAL_ID" \
  -Root 'C:\bench\calibration-001' -Exe 'C:\bench\ranges-three-way.exe' \
  -ExpectedSha256 "$EXPECTED_SHA" -GuestCpu 0
```

The calibration phase runs `c-first`, `reference-first`, `rust-first` and is
excluded from effect estimates. If its raw evidence fails any gate, diagnose
the cause before starting a new experiment. Do not select runs by result.
For comparison, use a new ID/root and link the calibration ID:

```sh
python3 hostmonitor.py --output-dir "$HOST_CMP" --pid "$QEMU_PID" \
  --qmp-socket "$QMP_SOCKET" \
  --expected-cpu "$HOST_CPU" --session-id "$CMP_ID" --phase comparison -- \
  ssh "$WINDOWS_HOST" powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\bench\run.ps1' -Phase comparison -SessionId "$CMP_ID" \
  -Root 'C:\bench\comparison-001' -Exe 'C:\bench\ranges-three-way.exe' \
  -ExpectedSha256 "$EXPECTED_SHA" -GuestCpu 0 \
  -CalibrationSessionId "$CAL_ID"
```

The comparison runs six three-order blocks in this predetermined shuffled
chronological sequence: `CAB`, `CBA`, `ABC`, `BAC`, `ACB`, `BCA`, where A is
`c-first`, B is `reference-first`, and C is `rust-first`. The sequence was
fixed before timing; each block still contains all three orders.
Every process uses Criterion's fixed 100 samples, 2-second warmup, 5-second
measurement and 95% confidence interval per case. The guest deadline defaults
to 5400 seconds; the host wrapper defaults to 5700 seconds. `-Smoke` records
the preflight, then runs one `--test` process
to exercise the wrapper. Smoke results are always rejected as timing evidence.

To check the Windows launch wrapper without benchmark timing, copy
`test_run.ps1` beside `run.ps1` and run it with a fresh probe root:

```powershell
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\bench\test_run.ps1 `
  -Runner C:\bench\run.ps1 -Root C:\bench\probe-001
```

The probe compiles a small temporary child under that root. It checks the
child's inherited initial affinity, parent affinity restoration, output from
a failing child, and detection of an external child affinity change. Its
results are diagnostics, not timing evidence.

Copy both fresh Windows result roots to Linux without changing any files.
Then validate the raw guest and host roots together. The assessor requires the
comparison's actual calibration session ID and rechecks all calibration files:

```sh
python3 assess.py --root "$GUEST_CAL" --host-root "$HOST_CAL" \
  --expected-sha256 "$EXPECTED_SHA" > calibration-report.json
python3 assess.py --root "$GUEST_CMP" --host-root "$HOST_CMP" \
  --calibration-root "$GUEST_CAL" --calibration-host-root "$HOST_CAL" \
  --expected-sha256 "$EXPECTED_SHA" > comparison-report.json
```

The assessor exits nonzero if any run, exit code, executable hash, metadata,
sample, confidence interval, affinity check, host trace or memory event is
missing or invalid. It checks all 16 cases in every run, recomputes the
through-origin linear slope from all 100 raw samples, requires a slope 95% CI
halfwidth at most 5%, first/last 20 sample median ratio from 0.90 to 1.10,
and a baseline max/min ratio at most 1.10 across both phases. It applies no
baseline subtraction. A valid but inconclusive result exits zero. The report
includes all four preflight busy CPU fractions for each phase. Its performance
estimates describe this VM under those observed loads, not isolated-hardware
performance; CPU load can still add noise or confounding even when the raw
evidence passes the declared validity checks.

Each workload/pair ratio is the geometric mean of six block log ratios. Its
95% interval uses a t multiplier of 2.570581835636305 (five degrees of
freedom) across those six blocks. The report also gives each order's estimate.
An interval wholly below 0.98 is faster, wholly above 1.02 is slower, wholly
inside 0.98–1.02 is practically equivalent, and all other valid intervals are
inconclusive. The 12 pair/workload intervals are marginal, without a multiple
comparison adjustment. This analysis assumes the six block estimates are
approximately independent and normal; Criterion samples within one run are
not independent experiment runs.

## Interleaved comparison

The opt-in `paired` bench measures C, original Rust, revised Rust and two
identical call-only baselines in short balanced chunks. It reuses the same
datasets, oracle, direct batch functions, reference object and helper build
flags. It requires the original Rust reference; the default `ranges` bench
and sequential assessor above retain their original behavior.

The first Windows smoke used 64 batches per chunk. Its empty-clock p99 was
15.8–69.7 microseconds against fastest chunks of 79.5–81.6 microseconds,
which failed the unchanged 0.5% timestamp budget. No paired pilot or
comparison had run. That executable and its smoke evidence are preserved.
Protocol `interleaved-v2` increases chunks by 256 times and uses Criterion's
minimum ten Flat samples to keep the experiment bounded. V1 evidence is
rejected by the v2 assessor.

Build it explicitly, from this directory, with the cached toolchain:

```sh
export CARGO_TARGET_DIR="$(git rev-parse --show-toplevel)/build/range-benchmark/target"
export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=gcc
export BTRFS_RANGE_REFERENCE_OBJECT="$(git rev-parse --show-toplevel)/build/range-benchmark/artifacts/range-rust.o"
RUSTFLAGS='-C debuginfo=0' cargo +1.98.0 bench --offline --locked \
  --target x86_64-pc-windows-gnu --features paired --bench paired --no-run
```

Copy the printed `paired-*.exe` to a separate guest filename and record its
SHA-256. Require a fresh `CRITERION_HOME` for each invocation. Before timing,
exercise the actual executable in Windows with `--test` and, in a separate
fresh output directory, `--timer-probe`. The latter runs the oracle and
10,000 empty `Instant::now()`/`elapsed()` spans, writes their full integer
nanosecond distribution and nearest-rank p99 (sorted element 9,899) to
`inputs.json`, prints the probe summary and exits before Criterion. A zero
p99 is invalid. Each normal invocation repeats the same probe outside
Criterion timing. Smoke, probe and measured results identify their mode
explicitly; only measured results can pass assessment.

```cmd
set CRITERION_HOME=C:\bench\paired-smoke-001
set BTRFS_RANGE_ORDER=c-first
paired.exe --test
set CRITERION_HOME=C:\bench\paired-probe-001
paired.exe --timer-probe
```

Every requested Criterion iteration is one full cycle of ten rows, with five
chunks per row. A chunk calls one existing direct batch function 16,384 times;
each batch has 1,024 helper calls. Labels 0 through 4 are `c`, `reference`,
`rust`, `baseline-a`, `baseline-b`. Both baseline labels invoke the same
batch function. Every label receives ten chunks, 163,840 batches and
167,772,160 helper calls per cycle. Dispatch, schedule construction, collection,
allocation and file I/O are outside each chunk's timer.

The base row is `[0,1,4,2,3]`. Its five cyclic label rotations, followed by
the reversals of those five rows, balance every label twice at every
position and every directed label pair twice among within-row adjacencies.
Row boundaries do not have this balance. Each cycle shuffles only the ten
rows with descending Fisher-Yates and SplitMix64. The initial wrapping-u64
state is `0x6e5a4b139c20d7f1 XOR ((order_index+1)*0xd1b54a32d192ed03)
XOR (cycle_index*0x94d049bb133111eb)`, where the three existing orders have
indices 0, 1, 2. Each shuffle draw adds `0x9e3779b97f4a7c15`, then applies
`(v XOR (v>>30))*0xbf58476d1ce4e5b9`,
`(v XOR (v>>27))*0x94d049bb133111eb`, and `v XOR (v>>31)`, with wrapping
multiplication. Swap index `i` with `draw % (i+1)` for `i=9..1`.

`interleaved-v2` metadata records the schedule algorithm
`splitmix64-fisher-yates-fnv1a64-v1`. For each dataset, the separate
`paired-DATASET.json` sidecar preserves every warmup and measured custom
timer invocation: positive integer iterations, contiguous cycle start,
five exact integer nanosecond sums, five minimum chunk durations and a
schedule digest. The digest starts at FNV offset `0xcbf29ce484222325` for
each invocation; for every actual label byte, XOR then multiply by
`0x100000001b3` modulo 2^64. The assessor independently reconstructs it.
Before reconstructing any schedule, it limits each dataset's complete sidecar
to 1,024 invocations and 65,536 total cycles, including warmup. This bounds
work on corrupted evidence; the cycle limit permits over 54 trillion helper
calls per dataset. Exceeding either capacity invalidates the run.
No row log is needed. Criterion receives the exact sum of all five label
durations, without clock or baseline subtraction.

There is one composite Criterion case per dataset. The configuration is
fixed to Flat sampling, 10 samples, two-second warmup, a five-second
measurement target and 95% reported confidence intervals. Criterion may
exceed this target because each sample needs a complete cycle. Ten complete
cycles are expected to take 10–15 seconds per dataset on this VM. The final
10 sidecar records must exactly match
Criterion's final 10 iterations and summed
times (allowing only float representation of integer nanoseconds). Every
measured record must have the same positive integer iteration count.
Earlier warmup records remain available but are excluded from estimation.

Use unchanged `run.ps1` and `hostmonitor.py` with this executable and its
explicit expected hash. Preserve all guest launch-affinity, deadline,
preflight-memory, host-identity, topology and telemetry requirements above.
Run one fresh excluded three-order calibration pilot. If it passes the
paired assessor, run the same eighteen-process comparison in predetermined
`CAB,CBA,ABC,BAC,ACB,BCA` blocks and link the pilot session. No parallel VM
benchmarks, host builds or tests during measured runs. Do not retry failed
pilots to select favorable outcomes or change the gates after inspecting
effects. Preserve any failed evidence and report the validity failure.

```sh
python3 paired_assess.py --root "$GUEST_CAL" --host-root "$HOST_CAL" \
  --expected-sha256 "$PAIRED_SHA" > paired-calibration-report.json
python3 paired_assess.py --root "$GUEST_CMP" --host-root "$HOST_CMP" \
  --calibration-root "$GUEST_CAL" --calibration-host-root "$HOST_CAL" \
  --expected-sha256 "$PAIRED_SHA" > paired-comparison-report.json
python3 -m unittest discover -s . -p 'test_*.py'
```

The separate paired assessor rejects sequential, stale, missing, extra or
malformed case evidence. It checks all integer sums, equal label work,
schedule digests, mean estimates and 95% interval metadata. In every run,
probe p99 must be at most 0.5% of the minimum observed chunk duration,
including warmup minima. This bounds a diagnostic for additive timestamp
cost relative to the 2% equivalence objective; it neither subtracts cost
nor guarantees an interruption-free chunk. The last-2/first-2 median
paired-ratio drift must be in `[0.90,1.10]` for A/A and all three helper
pairs. Two samples at each end preserve equal exposure but provide a
coarser drift diagnostic than twenty. Expected chunk durations around
20 milliseconds separate labels by tens of milliseconds, and a complete
cycle spans around one second. Common-load cancellation may weaken;
the drift and A/A controls still have to demonstrate adequate stability.
The pilot additionally requires each A/A run ratio in `[0.98,1.02]`
as a coarse futility check, not proof of equivalence. No interval based on
adjacent Criterion samples is an inference gate. Absolute baseline spread
is reported and does not gate this paired estimator. The sequential gates
remain unchanged.

A process ratio is total measured duration for A divided by total measured
duration for B. Absolute nanoseconds per call divide duration totals by
the total equal helper-call count. Sample ratios are drift diagnostics.
The comparison averages three process log ratios per block and uses the
same six-block t interval (multiplier 2.570581835636305). Each workload's
final A/A 95% interval must lie wholly in `[0.98,1.02]`; otherwise the entire
comparison is invalid. Helper intervals use the faster, slower, equivalent
or inconclusive classification above. All twelve helper intervals are
marginal, with no familywise significance claim. Raw absolute times,
per-order estimates, full samples and probe distributions remain part of
the evidence. These results describe elapsed VM helper costs for these
four datasets; A/A cannot exclude every helper-specific interaction or
common additive overhead.

## Observed VM result, 2026-10-21

The memory investigation reproduced cgroup limit pressure from qcow2 host
file-cache charges. Raising the limit alone did not remove it. A controlled
4 GiB guest with an 8 GiB cgroup and `cache=none` ran without new limit or
OOM events; the final pilot used an 8 GiB guest and 12 GiB cgroup for extra
headroom. Its peak charge was 8.106 GiB, with no new limit or OOM events.
Direct I/O addressed the observed memory pressure. It did not establish
adequate timing precision.

The final `interleaved-v2` pilot completed all three orders in 251.043 seconds
on Windows build 26300.9711 under QEMU/KVM. The benchmark ran on guest
CPU0, whose vCPU thread was pinned to a separate host physical core from
the other QEMU threads and monitor. The executable SHA-256 was
`f364049d7f32f2188d9fa9c0f76ce8b9a0525786bcc44968532b6b3e06573b9e`.
Its C, original Rust, revised Rust and baseline objects were byte-identical
to the earlier three-helper build; no production helper changed.

All 48 paired drift checks and all 12 timestamp-budget checks passed.
The timestamp p99 was 0.0743–0.0755% of the minimum chunk duration, below
the 0.5% budget. All executable hashes, raw sample bindings, schedules,
affinities, exits and host/guest envelope checks also passed. One of twelve
A/A point controls failed: the `rust-first` schedule's `mixed` dataset gave
`baseline-a/baseline-b = 0.9790613301`, below the required 0.98. The identical
function measured 2.094% less elapsed time under one label, exceeding the
2% pilot budget. Its totals were 2,026,668,800 ns and 2,070,012,100 ns for
1,677,721,600 calls per label.

The assessor therefore rejected the pilot. The eighteen-run comparison was
not started, no failed samples were discarded, and the gates were not
relaxed. These data do not support a reliable C/original-Rust/revised-Rust
ranking at the intended precision. Raw pilot evidence and the rejection
are retained locally as session `paired-calibration-20261021c`. A future
experiment needs a separately justified way to improve timing precision
and must repeat calibration before making performance claims.
