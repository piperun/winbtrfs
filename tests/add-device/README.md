# Add-device status regressions

From the repository root:

```sh
python3 tests/add-device/check_add_device.py . --build-dir /tmp/winbtrfs-add-device/fixed
python3 tests/add-device/check_add_device.py . --init zero --build-dir /tmp/winbtrfs-add-device/zero
UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/add-device/check_add_device.py . --sanitize --build-dir /tmp/winbtrfs-add-device/asan
```

The runner extracts the actual `add_device` from `add-device.c`, or from
`fsctl.c` before the handler moved, and the real Btrfs on-disk layout. It
adds a 2 MiB non-disk device to a mounted volume, once with every pool
allocation succeeding and once with each of the seven allocations failing in
turn: the PnP name, the device, the device item, the device statistics, the
zero buffer for the first megabyte, the volume child and its name.

Success must return `STATUS_SUCCESS`, add the device and the volume child,
count the new device in the superblock, keep one reference to the device's
file object and send one volume size-change notification. Each allocation
failure must leave the device lists and superblock unchanged, release the
file object and send no notification. The six allocations `add_device` makes
itself must return `STATUS_INSUFFICIENT_RESOURCES`. A failed PnP-name
allocation returns `STATUS_NOT_FOUND`, because the driver's name lookup
reports that for every failure.

`--revision 96fe76d4` returns success when the device-item allocation fails:
7/8 scenarios pass. The corrected handler passes 8/8 with patterned and zero
locals and ASan/UBSan.

Privilege checks, device I/O controls, tree insertion and lookup, device
writes, PnP registration and the mount manager are modeled. The device and
volume-child structures are not freed on `add_device`'s failure paths, and
the PnP name is not freed on its five early returns after the lookup, so
leak detection is off. Each scenario has a five-second timeout and no core
dumps.
