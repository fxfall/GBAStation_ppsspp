# ROMX 0.2.0 writer and mutable commit API

## Immutable container writer

`romx_writer_write_io_entries` and `romx_writer_write_path_entries` create
native ROMX 0.2.0 containers. Each input becomes one RIDX entry. Exactly one
entry must set `ROMX_RIDX_ENTRYPOINT`, and its `format_id` must be non-zero.
`ROMX_RIDX_HAS_CRC32` independently enables a stored CRC32 for that entry.

The writer streams entry bytes without compression. It physically writes the
entrypoint first at payload offset zero, writes the remaining entries, builds
RIDX, then writes optional metadata, optional PNG cover, immutable alignment
padding, optional mutable capacity, and the 128-byte footer. The completed
temporary file is published atomically.

Initialize `romx_writer_options_t` with `ROMX_WRITER_OPTIONS_INIT` and set a
registered `platform_id` and `launch_format_id`.

- `ROMX_WRITER_IMMUTABLE_SHA256` stores the optional immutable-range digest.
- `ROMX_WRITER_REPLACE_EXISTING` permits atomic destination replacement.
- `ROMX_WRITER_DURABLE` flushes the completed temporary file before publish.
- `ROMX_WRITER_PROBE_PAYLOAD` asks libromx to fill a missing metadata region
  and/or cover from the entrypoint when the format has extractable information.

Probing never overrides caller-supplied metadata or cover. It is best-effort;
unsupported, absent, encrypted, damaged, or non-text header content remains
absent. An extracted image must still pass the normal PNG validator.

For mutable storage, `mutable_capacity` must be a multiple of 4096 and at least
12288. `mutable_entry_capacity` must be a positive multiple of eight; zero
selects eight slots. Capacity is reserved once and does not enlarge during
later commits.

## Metadata and cover

Metadata is an optional in-memory strict UTF-8 JSON object conforming to the
ROMX 0.2.0 schema. libromx does not rewrite caller metadata and does not invent
database values. The cover is an optional PNG callback/path input and is
embedded byte-for-byte after structural validation. JPEG/WebP/GIF/BMP
conversion and resizing belong in a frontend or image adapter.

## Mutable in-place commits

`romx_mutable_write_io_path` and `romx_mutable_write_path` explicitly create or
overwrite one opaque SAVE, CHEAT, STATS, or PRIVATE object inside the reserved
mutable region. `romx_mutable_delete_path` explicitly deletes one object.

Writes lock the container, calculate the source CRC32, and use the required
three-stage protocol:

1. write and flush a valid `WRITING` directory entry;
2. overwrite the object's fixed-capacity extent and flush its bytes;
3. write and flush the final `ACTIVE` entry.

The third step is the commit point. Delete similarly commits `DELETING` before
clearing the directory slot. Every mutable commit is durable; the options
`flags` field is reserved and must be zero. A replacement keeps the original
extent and capacity and increments its generation. A new object may request a
larger fixed `data_capacity`, rounded to 64-byte alignment.

Mutable commits never change file size, move the footer, or recalculate the
optional immutable SHA-256. Insufficient capacity returns
`ROMX_E_MUTABLE_NO_SPACE`. An interrupted transaction may quarantine the
affected slot, but immutable game content remains readable.

## SAVE/CHEAT bundles and STATS

`romx_mutable_bundle_write_path_entries` accepts an explicit list of regular
source files and normalized relative paths. It sorts paths canonically,
rejects traversal, case-folding collisions, symlinks and non-regular files,
and streams an uncompressed `RMBL` object through the existing durable mutable
transaction. It does not create a tar file or scan a directory.

`romx_mutable_bundle_open` validates the outer object CRC32, bundle header,
directory, path table, zero padding, and every file CRC32 before exposing
entries. The bundle borrows its reader.

`romx_mutable_stats_serialize_json` and `romx_mutable_stats_parse_json`
implement the strict `romx.stats` version 1 schema.
`romx_mutable_stats_write_path` serializes and commits it in one operation.
SAVE, CHEAT, STATS, and PRIVATE remain separate mutable objects with independent
fixed extents; one namespace cannot borrow another object's capacity.
