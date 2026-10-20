# PnP usage notification regressions

```sh
python3 tests/pnp/check_pnp.py . --build-dir /tmp/winbtrfs-pnp
python3 tests/pnp/check_pnp.py . --init zero --sanitize --build-dir /tmp/winbtrfs-pnp
```

The runner extracts the production child usage notification handler, its
completion routine and context directly from `src/pnp.c`. The host model tracks
each allocated IRP until completion and release. Lower devices complete
synchronously, during the modeled wait, or before returning `STATUS_PENDING`.
The last case verifies that dispatch cannot overwrite the final completion
status.

The 222 scenarios cover empty, single-device and multiple-device child lists,
missing child devices, entering and leaving a usage path, lower-device
rejections, and first/second allocation failures. Each case checks the returned
status, forwarding order, copied notification parameters, completion waits,
IRP allocation balance and released child locks. Rejection of a later child
reverses earlier successful notifications in reverse order, for both additions
and removals. Six scenarios reject a compensating notification and verify that
rollback continues while preserving the original failure. Compensation is best
effort when another stack rejects the reversal; the driver logs that failure.
The model reclaims baseline leaks after recording their ownership failures.

With `--revision ec32d391`, 108/222 scenarios pass, with 108 IRP ownership
failures and 16 incorrect final statuses. Newly allocated requests start with
poisoned status fields in the model to verify initialization before
forwarding. The corrected implementation passes 222/222 with patterned and
zero initialization, with and without ASan/UBSan.
LeakSanitizer is disabled under sandbox tracing; allocation balance is counted
explicitly. Compilation is limited to 30 seconds and each run to 10 seconds.

These are deterministic host service models, not Windows scheduling or PnP
integration tests. Safe VM boot and ordinary device enumeration check driver
loading and routing only; they do not prove child-request ownership or the
completion timing cases. Live paging/hibernation changes and lower-driver
failure injection are not required for these regressions.

Microsoft documents caller-owned request completion in
[Implementing an IoCompletion Routine](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/implementing-an-iocompletion-routine)
and [IoFreeIrp](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iofreeirp).
Related-device rollback is required by
[IRP_MN_DEVICE_USAGE_NOTIFICATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-device-usage-notification).
