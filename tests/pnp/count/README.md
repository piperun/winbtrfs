# Filesystem usage-path count regressions

```sh
python3 tests/pnp/count/check_count.py . --build-dir /tmp/winbtrfs-count
python3 tests/pnp/count/check_count.py . --init zero --sanitize --build-dir /tmp/winbtrfs-count
```

The runner extracts the production filesystem usage notification handler and
its completion routine. The 120 host scenarios cover paging, dump and hibernation
files, two unknown usage types, additions and removals, synchronous and pending
completion, completion before a pending dispatch return, and lower-device
success/failure, both first/last usage paths and multiple existing paths.

The model verifies that an addition raises the count while still pending, so
the dismount guard continues to see the pending usage. Removals decrement the
count. Failed notifications reverse the original count change. It also checks
pending propagation and clears the lower stack location before completion to
verify that the completion uses the preserved upper stack parameters.

The previous handler passes 54/120 scenarios. The corrected handler passes
120/120 with patterned and zero initialization, with and without ASan/UBSan.
The runner accepts `--revision` for committed production code. Compilation is
limited to 30 seconds and execution to 10 seconds. All I/O and completion are
modeled; live paging/hibernation changes and lower-driver failure injection are
not part of the host or VM checks.

The count is changed on the downward path and reversed if the lower stack
rejects the request, following the completion contract in Microsoft's
[IRP_MN_DEVICE_USAGE_NOTIFICATION documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-device-usage-notification).
Pending status is propagated as required by
[Implementing an IoCompletion Routine](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/implementing-an-iocompletion-routine).
