# ROMX 0.2.0 integration guide

## C, C++, and Rust

Install libromx and consume its exported target:

```cmake
find_package(romx CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ROMX::romx)
```

Include `<romx/romx.h>` from C99 or later. C++ applications may use the C ABI
or the optional header-only `<romx/romx.hpp>` wrapper. Rust and other FFI
consumers should bind only `romx/romx.h`. Initialize every public structure
with its matching `*_INIT` macro.

## Launch data paths

Open the container once and inspect `romx_info_t`. No launch path copies the
complete concatenated payload merely to open a game.

`romx_reader_get_payload_io` returns a borrowed positional-read view of the
RIDX entrypoint. For a single-file game this is the native ROM or disc image.
For a multi-file game it can be a small descriptor rather than the entire
payload region.

For multi-file content, enumerate or resolve virtual paths with
`romx_reader_get_entry*` and `romx_reader_find_entry`, then open them with
`romx_vfs_file_open`. The frontend exposes the stored descriptor and referenced
tracks through this VFS. libromx does not extract the set at launch.

`romx_reader_map_payload` maps only the entrypoint for path-backed readers.
Mapping is intended for a single-file entrypoint and is not a replacement for
the multi-file VFS.

## Validation and startup cost

Opening validates footer, region, RIDX, and mutable structure with bounded
reads. It does not automatically scan all game bytes.

- `ROMX_VALIDATE_ENTRY_CRC32` scans entries that declare `HAS_CRC32`.
- `ROMX_VALIDATE_IMMUTABLE_SHA256` scans the immutable range only when the
  footer declares SHA-256.
- Mutable object CRC32 is checked before an ACTIVE object is returned/opened.

The immutable hash excludes mutable bytes and the footer, so updating a save
does not rehash a multi-gigabyte image.

## Mutable restore and explicit commit

Use `romx_reader_get_mutable_status` before enumeration. Opaque objects remain
available through `romx_mutable_file_open`.

For interoperable SAVE/CHEAT file sets, use `romx_mutable_bundle_open`,
enumerate with `romx_mutable_bundle_get_entry*`, and stream original file
bytes with `romx_mutable_bundle_read_entry`. Opening validates the complete
bundle and all per-file CRC32 values. Extract to staging and commit only the
listed paths; never replace a shared frontend save root.

Use `romx_mutable_stats_read` and `romx_mutable_stats_serialize_json` for the
strict versioned STATS profile. Cumulative runtime synchronization should
re-read the current object and add only the current session delta.

Writing back is a separate explicit action. Call
`romx_mutable_bundle_write_path_entries` for caller-selected regular files,
`romx_mutable_stats_write_path` for statistics, or the generic
`romx_mutable_write_io_path`/`romx_mutable_write_path` APIs for opaque data.
Call `romx_mutable_delete_path` for explicit deletion. libromx performs the
in-place durable transaction, while UI,
destination directories, synchronization policy, and user confirmation remain
frontend responsibilities. Save states are outside the ROMX 0.2.0 namespaces.

A mutable error never invalidates or blocks immutable payload access.

## Optional entrypoint probing

`romx_probe_open_io` and `romx_probe_open_path` inspect a caller-declared file
format for embedded title, serial, and artwork/icon data. The native writer can
invoke the same mechanism with `ROMX_WRITER_PROBE_PAYLOAD` when metadata or
cover is missing. Probe results are descriptive and never replace footer/RIDX
platform and format declarations.

## Lifetimes and concurrency

Borrowed entrypoint views and VFS/mutable cursors require the reader to outlive
them. Path-backed readers use positional I/O. A custom `romx_io_t` source must
keep `user_data` valid, report a stable size, and make `read_at` thread-safe if
used concurrently.
