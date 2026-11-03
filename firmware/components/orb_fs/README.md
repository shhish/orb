# Orb app storage

`orb_fs_mount()` mounts the `storage` partition at `/storage` at boot. Migration
must erase the new storage range once. Only a failed mount whose **entire**
partition reads `0xff` is formatted; read failures or any nonempty media leave
the partition untouched. A corrupted filesystem therefore disables app saves
while the shell/recovery services continue. There is no automatic destructive
repair or format API.

Apps get separate `/storage/apps/<id>` directories, created lazily on launch or
first write. Public APIs accept lowercase IDs/keys (`a-z`, `0-9`, `_`, `-`), never
paths. IDs are at most 24 bytes and keys at most 32. All access must use this API;
compiled apps must not bypass it through raw VFS writes. Limits are eight app
directories, 32 committed files/app, 64 KiB/file, 256 KiB/app, and 128 KiB of free
filesystem reserve. Staging requires room for the entire replacement in addition
to the old committed file. These are admission bounds, not preallocated space.

Writes are serialized by one static mutex, write a reserved `.pending` file,
`fsync()`, close, and atomically rename it over the destination. The old file is
retained on write/sync/rename failure. A reset before rename leaves the old
committed file; after rename LittleFS's metadata transaction commits the complete
replacement. A stale `.pending` file is ignored on reads and truncated on the next
write, bounding interrupted-write space to one file/app. `orb_fs_remove()` uses a
LittleFS metadata transaction. Batch/debounce saves in the app; do not save every
animation tick. LittleFS's default 512 block cycles enables wear levelling and
copy-on-write metadata handles power interruption. Physical power-cut endurance
remains a lead/device test.

Dependency is pinned to `joltwallet/littlefs ==1.22.3` (registry commit
`bde79e13be971a1f06d5e5f55e6f3e8568fa32b5`), compatible with ESP-IDF >=5.0. Checked
the downloaded component's `LICENSE` (MIT, Brian Pugh) and
`src/littlefs/LICENSE.md` (BSD-3-Clause, Arm and the LittleFS authors); preserve
both notices when distributing the managed component. Upstream:
<https://github.com/joltwallet/esp_littlefs/tree/v1.22.3>.

The ESP32 build map records 100 bytes of wrapper static BSS (including alignment),
plus 16 bytes of LittleFS global BSS; there is no wrapper heap or task. This is
`firmware/build/orb.map` around `s_ready`/`s_mutex_memory`/`_efs`, not a live device
measurement. The blank scan uses a 256-byte stack buffer; filesystem operations
use bounded stack paths. With upstream defaults, LittleFS has two 512-byte caches
and a 128-byte lookahead, plus filesystem/VFS bookkeeping; an open file adds a
512-byte cache and bookkeeping. The wrapper serializes operations and holds at
most one file or directory open at a time. Actual heap use must be measured on the
device; these source-level cache sizes are not a device heap measurement.

Host checks run production wrapper and production ESP mount logic under
ASan/UBSan with fault-injected writes, sync, rename, mount, partition reads and
format calls:

```sh
make -C firmware/components/orb_fs/tests/host test
```

These check isolation, traversal rejection, app/file/count quotas, reserve,
retry/short writes, stale staging recovery, preserving committed data on failure,
and blank-only format refusal including a nonblank byte at the end of media.
