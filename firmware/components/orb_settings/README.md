# Preference persistence service

`orb_settings` owns the existing version-2, 16-byte Faces preference record and
its `faces/preferences` storage key. It has no LVGL or presentation dependency.
The face/provider enums and validation bounds retain their existing values.

Main calls `orb_settings_init()` after mounting storage and before creating the
UI, then calls `orb_settings_service()` outside the LVGL lock. Initialization
reads once without creating a namespace. Unavailable storage, read failures and
invalid record sizes or values silently retain the existing defaults.

The UI submits a copied value only when an existing save trigger fires. Eight
fixed slots hold requests and retained results. Every admitted request is written
in order; the service performs one write per call, with no debounce, coalescing
or automatic retry. Queueing reports acceptance through a ticket, while the
ticket result reports pending, saved or failed separately. The service revision
exposes changes to aggregate saving/error state. An older completion cannot
replace the status of a newer request.

Consumers release each ticket after observing its result or when its view is
destroyed. Releasing a pending ticket detaches only its observer. The service
still writes its copied record, then recycles that slot. Completed observed
results remain valid until release, so another request cannot overwrite them.
Current preferences contain only a successfully completed value. A successful
external persistence adapter uses `orb_settings_faces_accept()` to publish its
value without causing a second write.

Ordinary host presenters preserve their existing memory-only save behavior:
requests complete synchronously and the service pump performs no filesystem
work. Storage-enabled tests use `ORB_SETTINGS_TEST_STORAGE` and exercise the same
queued path as firmware against the production `orb_fs` implementation with an
isolated temporary directory.

Run the standalone, sanitizer-enabled suite with:

```sh
nice -n 10 make -C firmware/components/orb_settings/tests/host -j4 test
```

Coverage includes default/validation/ABI parity, immediate host completion,
queued writes without UI I/O, exact bytes and key, FIFO order, detached view
lifetime, retained results and admission failure, a request arriving during a
failed write, explicit retry, prepare/write failure, and valid/corrupt/truncated
startup records.
