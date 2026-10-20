# Volume probe ownership regressions

```sh
python3 tests/devctrl/check_probe.py . --build-dir /tmp/winbtrfs-probe
python3 tests/devctrl/check_probe.py . --init zero --sanitize --build-dir /tmp/winbtrfs-probe
```

The runner extracts `probe_volume` from the production device-control handler.
Modeled services return a caller-owned PnP name and retain copies during arrival
processing. The eight scenarios cover input validation, privilege denial,
device-open/name lookup failures, disk and volume arrival, and a disk-property
refresh failure. They check file-object references, arrival ordering, borrowed
name lifetime, retained name content and allocation balance. No devices are
actually removed or probed by the host tests.

Before the name release, 5/8 scenarios pass; successful lookup paths each leak
one name allocation. The corrected handler passes 8/8 with patterned and zero
initialization, with and without ASan/UBSan. The runner also accepts `--revision`
to check committed production code. Modeled baseline leaks are reclaimed after
recording the failure. LeakSanitizer is disabled under sandbox tracing;
allocation balance is checked explicitly. Compilation is limited to 30 seconds
and execution to 10 seconds.

Ownership matches `get_device_pnp_name_guid` in `src/btrfs.c`, which allocates
the returned name. Arrival code copies any name it retains: `add_volume_device`
in `src/volume.c` and the encrypted-volume callback registration in
`src/search.c`. The probe releases its own allocation after removal and arrival
processing have returned.

A safe VM boot and read-only enumeration check loading and routing. Live
re-probing can remove and re-add device state; it is not part of those smoke
checks, and host ownership checks do not claim live kernel pool measurements.
