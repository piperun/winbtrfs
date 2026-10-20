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
`inputs.json` under `CRITERION_HOME`, including the seed and FNV-1a 64-bit
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

The build script chooses `x86_64-w64-mingw32-{gcc,ar,nm}` for Windows and
`{gcc,ar,nm}` for Linux, with `BTRFS_RANGE_{CC,AR,NM,RUSTC}` overrides. The C
helper is compiled with `-O2 -fno-lto`; its symbol alone is renamed. The Rust
leaf uses `opt-level=2`, overflow checks, panic abort, one codegen unit, no
red zone, no LTO and no debug info. The benchmark profile has no debug info,
no LTO and one codegen unit. No CPU-specific tuning is used.

Copy the `ranges-*.exe` file printed by the build command to the Windows VM.
Set `CRITERION_HOME` to a dedicated output directory for each run. Leave
`BTRFS_RANGE_ORDER` unset or set it to `c-first` for normal registration order;
set it to `rust-first` for reversed order. The `--test` smoke mode runs the
preflight and Criterion's test mode. A full run omits `--test`:

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

Run the two measurements serially without compiling or doing other heavy work
on the host. Criterion uses 100 samples, 2 seconds warmup, 5 seconds measurement
and 95% confidence intervals. Preserve each directory's `new/sample.json`,
`new/estimates.json`, `new/benchmark.json` and `inputs.json`. The result roots
must differ so a run cannot become the other's statistical baseline.

Normalize completed runs with:

```sh
python3 summarize.py PATH_TO_C_FIRST PATH_TO_RUST_FIRST > summary.json
```

The summarizer divides Criterion's batch nanoseconds and interval endpoints by
1024, preserves each interval's actual `confidence_level`, prefers its slope
estimate when present, and reports a point Rust/C ratio. It does not infer a
confidence interval for that ratio. Inspect raw samples
and order sensitivity before drawing a performance conclusion. The baseline
is reported as its own raw time per call.
