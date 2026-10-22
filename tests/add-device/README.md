# Add-device status and ownership regressions

From the repository root:

```sh
python3 tests/add-device/check_add_device.py . --build-dir /tmp/winbtrfs-add-device/fixed
python3 tests/add-device/check_add_device.py . --init zero --build-dir /tmp/winbtrfs-add-device/zero
python3 tests/add-device/check_add_device.py . --sanitize --build-dir /tmp/winbtrfs-add-device/asan
```

The runner extracts the actual `add_device` from `add-device.c`, or from
`fsctl.c` before the handler moved, and the real Btrfs on-disk layout. It
adds a 2 MiB non-disk device to a mounted volume, once with every pool
allocation succeeding and once with each of the eight allocations failing in
turn: the PnP name, the device, its free-space entry, the device item, the
device statistics, the zero buffer for the first megabyte, the volume child
and its name. It also fails each pre-transfer device check, tree operation and
first-megabyte write, and fails the flush after ownership transfers.

Success must return `STATUS_SUCCESS`, add the device and the volume child,
count the new device in the superblock, keep one reference to the device's
file object and send one volume size-change notification. Each allocation
failure before linking must leave the device lists and superblock unchanged,
release the file object and send no notification. The seven allocations after
PnP lookup must return `STATUS_INSUFFICIENT_RESOURCES`. A failed PnP-name
allocation returns `STATUS_NOT_FOUND`, because the driver's name lookup
reports that for every failure.

Pool allocations and frees are counted, including the entry added to
`dev->space`. Tree-inserted items are freed by the modelled tree, while the
test releases device and child structures after a successful transfer. PnP
registrations are counted separately, with both PnP unregistration APIs
exercised after a child-name allocation failure. Every case must finish with
no unowned pool allocations or registrations; sanitizer runs also enable leak
detection.

`--revision 96fe76d4` and `--revision ff86d985` pass 5/22 scenarios. The
former also returns success when the device-item allocation fails. The fixed
handler passes 22/22 with patterned and zero locals, plain and ASan/UBSan.

Privilege checks, device I/O controls, tree insertion and lookup, device
writes, PnP registration and the mount manager are modeled. Each scenario has
a five-second timeout and no core dumps.
