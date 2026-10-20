# Volume result regressions

```sh
python3 tests/volume/check_volume.py . --build-dir /tmp/winbtrfs-volume
python3 tests/volume/check_volume.py . --init zero --sanitize --build-dir /tmp/winbtrfs-volume
```

The runner extracts the production writability and filesystem-enumeration
handlers and the actual filesystem-device output layout. Modeled devices
return success, write protection or a device error. Enumeration uses both
fresh and previously populated output buffers, including connected and absent
devices. Assertions cover the computed result, missing flags, names, identities
and balanced resources. The unchanged `be01e7d0` passes 5/8 scenarios; the fix
passes 8/8 with patterned and zero initialization and ASan/UBSan.

These fixtures do not issue Windows device controls or emulate removable-device
scheduling. Live checks should query the finished driver's normal volume and
device enumeration APIs; negative lower-device statuses remain modeled unless
a reversible, task-owned Windows fixture can produce them. Each run has a
ten-second limit and disables core dumps. LeakSanitizer is disabled under
tracing; no pool allocation is involved in these modeled handlers.
