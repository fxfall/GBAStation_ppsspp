# libromx

`libromx` is a portable C99 implementation of the ROMX 0.2.0 container
format. It is independent of emulator cores, frontends, GUI frameworks,
databases, and RetroArch playlists. The public ABI contains no C++ STL types.

ROMX 0.2.0 is the only wire format implemented by this branch.

## Features

- fixed 128-byte footer and overflow-checked region parsing;
- mandatory RIDX index with one launch entrypoint and multiple virtual files;
- direct entrypoint callback view and guarded memory mapping;
- random-access virtual files for CUE, GDI, M3U, tracks, and sidecars without
  extracting the complete payload;
- optional per-entry CRC32 and immutable SHA-256 validation;
- strict ROMX 0.2.0 metadata JSON and embedded PNG validation;
- native multi-entry streaming writer with atomic publication;
- optional payload probing when metadata or cover is absent;
- fixed-capacity mutable SAVE, CHEAT, STATS, and PRIVATE objects;
- deterministic uncompressed SAVE/CHEAT bundles with normalized paths and
  per-file CRC32;
- strict versioned STATS JSON parsing, serialization, and explicit commit;
- durable in-place mutable write, overwrite, and delete commits without moving
  the footer or rewriting immutable game data.

Opening a container performs bounded structural reads. Payload CRC32 and
immutable SHA-256 scans happen only when explicitly requested. A corrupt or
exhausted mutable region never prevents access to otherwise valid payload,
RIDX, metadata, or cover data.

Payload probing currently recognizes useful embedded headers and artwork from
GB/GBC, GBA, NDS, N64 byte-order variants, SNES, Mega Drive/Genesis/SMD,
PBP, and PSP ISO inputs. Results are best-effort: a failed probe leaves the
optional metadata or cover region absent. Image conversion is outside libromx.

## Build and test

```sh
cmake -S . -B build -DROMX_BUILD_TESTS=ON \
  -DROMX_CONFORMANCE_FIXTURE_DIR=/path/to/romx/tests/fixtures
cmake --build build
ctest --test-dir build --output-on-failure
```

When the specification repository is adjacent to this project, the fixture
path is detected automatically. Tests cover footer/RIDX parsing, multi-file
VFS access, the native writer, payload probing, immutable validation, and
mutable in-place commit behavior.

## Frontend integration

For a single-file game, the RIDX entrypoint is the native ROM or image. For a
multi-file game, it is normally a descriptor such as CUE, GDI, or M3U.

- Use `romx_reader_get_payload_io` for an entrypoint-only positional-read view.
- Use `romx_reader_map_payload` when a core accepts one memory buffer.
- Use `romx_vfs_file_open` to expose every RIDX path requested by a multi-file
  core.
- Use `romx_mutable_bundle_open` for interoperable SAVE/CHEAT file sets.
- Use `romx_mutable_stats_read` for the strict STATS profile.
- Use `romx_mutable_write_*` and `romx_mutable_delete_path` only after an
  explicit frontend save/delete action.

Borrowed callback views and cursors require their `romx_reader_t` to remain
open. A successful guarded mapping owns its mapping and survives reader close.

The normative format rules are maintained in the
[ROMX specification](https://github.com/fxfall/romx-container-spec). This
repository defines the library API and implementation behavior.
