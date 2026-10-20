# Pure byte-range clipping tests

```sh
python3 tests/range-math/check_math.py . --build-dir /tmp/winbtrfs-range-math
python3 tests/range-math/check_math.py . --init zero --sanitize --build-dir /tmp/winbtrfs-range-math
```

The portable production helper intersects half-open byte ranges. Reversed or
empty queries and empty intersections return zero and clear both output
endpoints. A nonempty intersection returns one. The caller supplies a valid,
aligned, writable output pointer. The helper has no allocation, filesystem
state or Windows dependency.

The test covers every combination of four inputs from 0 through 15, plus a
boundary grid around 32-bit, signed 64-bit and unsigned 64-bit limits. An
independent 128-bit oracle forms the mathematical extent end before clipping,
including lengths that would overflow a 64-bit sum. Guards surround the output
range. Additional properties check idempotence and clipping through a nested
query. Compile-time checks cover the fixed-width C ABI layout.

These tests validate arithmetic independently of the driver. The existing
query harness checks file classification, locking, range coalescing, output
capacity, returned byte counts and status handling. Each helper run has a
ten-second deadline; compilation is limited to thirty seconds.
