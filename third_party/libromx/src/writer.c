#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct writer_settings {
    romx_writer_flags_t flags;
    uint16_t platform_id;
    uint16_t launch_format_id;
    uint64_t mutable_capacity;
    uint32_t mutable_entry_capacity;
    uint64_t max_metadata_size;
    uint64_t max_cover_size;
    uint32_t max_cover_dimension;
    uint32_t io_chunk_size;
} writer_settings_t;

typedef struct writer_entry_state {
    uint64_t size;
    uint64_t offset;
    uint32_t crc32;
} writer_entry_state_t;

typedef struct path_input {
    FILE *file;
    uint64_t size;
} path_input_t;

typedef struct memory_input {
    const uint8_t *bytes;
    uint64_t size;
} memory_input_t;

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static int valid_io(const romx_io_t *io)
{
    return io != NULL && io->struct_size >= sizeof(*io) &&
        io->get_size != NULL && io->read_at != NULL;
}

static int valid_virtual_path(const char *path)
{
    const uint8_t *bytes = (const uint8_t *)path;
    size_t size;
    size_t index;
    size_t component = 0U;
    size_t bad_offset = 0U;
    if (path == NULL) return 0;
    size = strlen(path);
    if (size == 0U || size > ROMX_RIDX_PATH_CAPACITY ||
        bytes[0] == (uint8_t)'/' || bytes[size - 1U] == (uint8_t)'/' ||
        !romx_utf8_validate(bytes, size, &bad_offset)) return 0;
    for (index = 0U; index <= size; ++index) {
        if (index < size && bytes[index] != (uint8_t)'/') {
            if (bytes[index] == (uint8_t)'\\') return 0;
            continue;
        }
        if (index == component ||
            (index - component == 1U && bytes[component] == (uint8_t)'.') ||
            (index - component == 2U && bytes[component] == (uint8_t)'.' &&
                bytes[component + 1U] == (uint8_t)'.')) return 0;
        component = index + 1U;
    }
    return 1;
}

static int ascii_fold_equal(const char *first, const char *second)
{
    for (;;) {
        unsigned char a = (unsigned char)*first++;
        unsigned char b = (unsigned char)*second++;
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z') a = (unsigned char)(a + 32U);
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z') b = (unsigned char)(b + 32U);
        if (a != b) return 0;
        if (a == 0U) return 1;
    }
}

static int load_settings(const romx_writer_options_t *options,
    writer_settings_t *settings)
{
    const romx_writer_flags_t allowed = ROMX_WRITER_IMMUTABLE_SHA256 |
        ROMX_WRITER_REPLACE_EXISTING | ROMX_WRITER_DURABLE |
        ROMX_WRITER_PROBE_PAYLOAD;
    memset(settings, 0, sizeof(*settings));
    settings->max_metadata_size = ROMX_DEFAULT_MAX_METADATA_SIZE;
    settings->max_cover_size = ROMX_DEFAULT_MAX_COVER_SIZE;
    settings->max_cover_dimension = ROMX_DEFAULT_MAX_COVER_DIMENSION;
    settings->io_chunk_size = ROMX_DEFAULT_IO_CHUNK_SIZE;
    settings->mutable_entry_capacity = UINT32_C(8);
    if (options == NULL || options->struct_size < sizeof(*options) ||
        (options->flags & ~allowed) != UINT32_C(0)) return 0;
    settings->flags = options->flags;
    settings->platform_id = options->platform_id;
    settings->launch_format_id = options->launch_format_id;
    settings->mutable_capacity = options->mutable_capacity;
    if (options->mutable_entry_capacity != UINT32_C(0)) {
        settings->mutable_entry_capacity = options->mutable_entry_capacity;
    }
    if (options->max_metadata_size != UINT64_C(0)) settings->max_metadata_size = options->max_metadata_size;
    if (options->max_cover_size != UINT64_C(0)) settings->max_cover_size = options->max_cover_size;
    if (options->max_cover_dimension != UINT32_C(0)) settings->max_cover_dimension = options->max_cover_dimension;
    if (options->io_chunk_size != UINT32_C(0)) settings->io_chunk_size = options->io_chunk_size;
    if (settings->platform_id == UINT16_C(0) || settings->platform_id == UINT16_C(0xffff) ||
        settings->launch_format_id == UINT16_C(0) || settings->launch_format_id == UINT16_C(0xffff) ||
        settings->io_chunk_size < UINT32_C(1024)) return 0;
    if (settings->mutable_capacity != UINT64_C(0)) {
        uint64_t directory_size;
        uint64_t data_offset;
        if (settings->mutable_entry_capacity < UINT32_C(8) ||
            settings->mutable_entry_capacity % UINT32_C(8) != UINT32_C(0) ||
            settings->mutable_capacity < UINT64_C(12288) ||
            settings->mutable_capacity % UINT64_C(4096) != UINT64_C(0)) return 0;
        directory_size = (uint64_t)settings->mutable_entry_capacity * UINT64_C(512);
        data_offset = UINT64_C(4096) + directory_size;
        if (data_offset >= settings->mutable_capacity ||
            data_offset % UINT64_C(4096) != UINT64_C(0)) return 0;
    }
    return 1;
}

static romx_result_t input_read_exact(const romx_io_t *input,
    uint64_t offset, void *buffer, uint64_t size, romx_error_t *error)
{
    uint64_t total = UINT64_C(0);
    while (total < size) {
        uint64_t count = UINT64_C(0);
        romx_result_t result = input->read_at(input->user_data, offset + total,
            (uint8_t *)buffer + (size_t)total, size - total, &count, error);
        if (result != ROMX_OK) return result;
        if (count == UINT64_C(0) || count > size - total) {
            return romx_error_set(error, ROMX_E_TRUNCATED, 0,
                offset + total, "writer input changed or ended early");
        }
        total += count;
    }
    return ROMX_OK;
}

static romx_result_t write_all(int descriptor, const void *bytes,
    size_t size, uint64_t offset, romx_error_t *error)
{
    size_t written = 0U;
    while (written < size) {
#if defined(_WIN32)
        int count = _write(descriptor, (const uint8_t *)bytes + written,
            (unsigned int)(size - written));
#else
        ssize_t count = write(descriptor,
            (const uint8_t *)bytes + written, size - written);
#endif
        if (count < 0) {
            if (errno == EINTR) continue;
            return romx_error_set(error, ROMX_E_WRITE, errno,
                offset + (uint64_t)written, "failed to write ROMX output");
        }
        if (count == 0) {
            return romx_error_set(error, ROMX_E_WRITE, 0,
                offset + (uint64_t)written, "zero-length ROMX output write");
        }
        written += (size_t)count;
    }
    return ROMX_OK;
}

static romx_result_t write_immutable(int descriptor, const void *bytes,
    size_t size, uint64_t offset, romx_sha256_context_t *sha,
    romx_error_t *error)
{
    romx_result_t result = write_all(descriptor, bytes, size, offset, error);
    if (result == ROMX_OK && sha != NULL)
        romx_sha256_update(sha, (const uint8_t *)bytes, size);
    return result;
}

static romx_result_t stream_entry(const romx_io_t *input, uint64_t size,
    int descriptor, uint64_t output_offset, uint32_t chunk_size,
    romx_sha256_context_t *sha, uint32_t *finished_crc,
    romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = UINT32_C(0);
    romx_result_t result = ROMX_OK;
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate writer buffer");
    if (finished_crc != NULL) crc = romx_crc32_begin();
    while (position < size) {
        uint64_t count = size - position;
        if (count > chunk_size) count = chunk_size;
        result = input_read_exact(input, position, buffer, count, error);
        if (result != ROMX_OK) break;
        result = write_immutable(descriptor, buffer, (size_t)count,
            output_offset + position, sha, error);
        if (result != ROMX_OK) break;
        if (finished_crc != NULL)
            crc = romx_crc32_update(crc, buffer, (size_t)count);
        position += count;
    }
    free(buffer);
    if (result == ROMX_OK && finished_crc != NULL)
        *finished_crc = romx_crc32_finish(crc);
    return result;
}

#if defined(_WIN32)
static wchar_t *to_wide(const char *path)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) <= 0) {
        free(wide); return NULL;
    }
    return wide;
}
#endif

static int remove_utf8(const char *path)
{
#if defined(_WIN32)
    wchar_t *wide = to_wide(path);
    int result = wide != NULL ? _wremove(wide) : -1;
    free(wide);
    return result;
#else
    return unlink(path);
#endif
}

static romx_result_t create_temporary(const char *destination,
    char **temporary_out, int *descriptor_out, romx_error_t *error)
{
    static const char suffix[] = ".romx-tmp-XXXXXX";
    size_t length = strlen(destination);
    char *temporary;
    int descriptor;
    if (length > SIZE_MAX - sizeof(suffix)) return ROMX_E_OUT_OF_MEMORY;
    temporary = (char *)malloc(length + sizeof(suffix));
    if (temporary == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY,
        0, ROMX_OFFSET_UNKNOWN, "failed to allocate temporary path");
    memcpy(temporary, destination, length);
    memcpy(temporary + length, suffix, sizeof(suffix));
#if defined(_WIN32)
    descriptor = -1;
    {
        unsigned int attempt;
        char *tail = temporary + length + 10U;
        for (attempt = 0U; attempt < 1000U; ++attempt) {
            wchar_t *wide;
            (void)snprintf(tail, 7U, "%06u", attempt);
            wide = to_wide(temporary);
            if (wide == NULL) break;
            if (_wsopen_s(&descriptor, wide,
                _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) descriptor = -1;
            free(wide);
            if (descriptor >= 0) break;
        }
    }
#else
    descriptor = mkstemp(temporary);
#endif
    if (descriptor < 0) {
        int code = errno;
        free(temporary);
        return romx_error_set(error, ROMX_E_WRITE, code,
            ROMX_OFFSET_UNKNOWN, "failed to create temporary ROMX output");
    }
    *temporary_out = temporary;
    *descriptor_out = descriptor;
    return ROMX_OK;
}

static romx_result_t close_output(int descriptor, int durable,
    romx_error_t *error)
{
    if (durable) {
#if defined(_WIN32)
        if (_commit(descriptor) != 0) {
#else
        if (fsync(descriptor) != 0) {
#endif
            int code = errno;
            return romx_error_set(error, ROMX_E_WRITE, code,
                ROMX_OFFSET_UNKNOWN, "failed to sync ROMX output");
        }
    }
#if defined(_WIN32)
    if (_close(descriptor) != 0)
#else
    if (close(descriptor) != 0)
#endif
        return romx_error_set(error, ROMX_E_WRITE, errno,
            ROMX_OFFSET_UNKNOWN, "failed to close ROMX output");
    return ROMX_OK;
}

static romx_result_t publish_temporary(const char *temporary,
    const char *destination, int replace, romx_error_t *error)
{
#if defined(_WIN32)
    wchar_t *source = to_wide(temporary);
    wchar_t *target = to_wide(destination);
    DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (source == NULL || target == NULL || !MoveFileExW(source, target, flags)) {
        DWORD code = GetLastError(); free(source); free(target); remove_utf8(temporary);
        return romx_error_set(error,
            code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? ROMX_E_EXISTS : ROMX_E_ATOMIC_RENAME,
            (int32_t)code, ROMX_OFFSET_UNKNOWN, "failed to publish ROMX output");
    }
    free(source); free(target);
#else
    if (replace) {
        if (rename(temporary, destination) != 0) {
            int code = errno; unlink(temporary);
            return romx_error_set(error, ROMX_E_ATOMIC_RENAME, code,
                ROMX_OFFSET_UNKNOWN, "failed to replace ROMX output");
        }
    } else {
        if (link(temporary, destination) != 0) {
            int code = errno; unlink(temporary);
            return romx_error_set(error, code == EEXIST ? ROMX_E_EXISTS : ROMX_E_ATOMIC_RENAME,
                code, ROMX_OFFSET_UNKNOWN, "failed to publish ROMX output");
        }
        unlink(temporary);
    }
#endif
    return ROMX_OK;
}

static romx_result_t memory_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    memory_input_t *input = (memory_input_t *)user;
    (void)error; *size = input->size; return ROMX_OK;
}

static romx_result_t memory_read_at(void *user, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    memory_input_t *input = (memory_input_t *)user;
    uint64_t count;
    (void)error;
    if (offset > input->size) return ROMX_E_RANGE;
    count = input->size - offset;
    if (count > size) count = size;
    if (count != 0U) memcpy(buffer, input->bytes + (size_t)offset, (size_t)count);
    *bytes_read = count;
    return ROMX_OK;
}

static romx_result_t write_zeroes(int descriptor, uint64_t size,
    uint64_t offset, romx_sha256_context_t *sha, romx_error_t *error)
{
    uint8_t zeroes[4096] = { 0 };
    uint64_t written = UINT64_C(0);
    while (written < size) {
        size_t count = (size - written) > sizeof(zeroes)
            ? sizeof(zeroes) : (size_t)(size - written);
        romx_result_t result = sha != NULL
            ? write_immutable(descriptor, zeroes, count, offset + written, sha, error)
            : write_all(descriptor, zeroes, count, offset + written, error);
        if (result != ROMX_OK) return result;
        written += count;
    }
    return ROMX_OK;
}

static romx_result_t build_mutable_header(uint8_t header[4096],
    const writer_settings_t *settings)
{
    uint64_t directory_size =
        (uint64_t)settings->mutable_entry_capacity * UINT64_C(512);
    uint64_t data_offset = UINT64_C(4096) + directory_size;
    uint32_t crc;
    memset(header, 0, 4096U);
    memcpy(header, "RMUT", 4U);
    write_le16(header + 0x04U, UINT16_C(1));
    write_le16(header + 0x06U, UINT16_C(4096));
    write_le32(header + 0x08U, UINT32_C(512));
    write_le32(header + 0x0CU, settings->mutable_entry_capacity);
    write_le64(header + 0x10U, UINT64_C(4096));
    write_le64(header + 0x18U, directory_size);
    write_le64(header + 0x20U, data_offset);
    write_le64(header + 0x28U, settings->mutable_capacity - data_offset);
    crc = romx_crc32_begin();
    crc = romx_crc32_update(crc, header, 4096U);
    crc = romx_crc32_finish(crc);
    write_le32(header + 0x34U, crc);
    return ROMX_OK;
}

romx_result_t romx_writer_write_io_entries(const char *destination,
    const romx_writer_io_entry_t *entries, uint32_t entry_count,
    const void *metadata_json, uint64_t metadata_size,
    const romx_io_t *cover, const romx_writer_options_t *options,
    romx_writer_report_t *report, romx_error_t *error)
{
    writer_settings_t settings;
    writer_entry_state_t *states = NULL;
    uint8_t *ridx = NULL;
    uint8_t *probed_metadata = NULL;
    uint8_t *probed_cover = NULL;
    romx_probe_t *probe = NULL;
    memory_input_t cover_memory;
    romx_io_t cover_memory_io = ROMX_IO_INIT;
    const romx_io_t *effective_cover = cover;
    const uint8_t *effective_metadata = (const uint8_t *)metadata_json;
    uint64_t effective_metadata_size = metadata_size;
    uint64_t cover_size = UINT64_C(0);
    uint64_t payload_size = UINT64_C(0);
    uint64_t ridx_size;
    uint64_t offset = UINT64_C(0);
    uint64_t immutable_padding = UINT64_C(0);
    uint64_t immutable_size;
    uint64_t file_size;
    uint32_t entrypoint = UINT32_MAX;
    uint32_t index;
    uint32_t supplied_report_size = UINT32_C(0);
    char *temporary = NULL;
    int descriptor = -1;
    romx_sha256_context_t immutable_context;
    uint8_t immutable_hash[32] = { 0 };
    uint8_t footer[ROMX_FOOTER_SIZE];
    romx_result_t result;

    romx_error_clear(error);
    if (destination == NULL || *destination == '\0' || entries == NULL ||
        entry_count == UINT32_C(0) || options == NULL ||
        !load_settings(options, &settings) ||
        (metadata_json == NULL) != (metadata_size == UINT64_C(0)) ||
        (cover != NULL && !valid_io(cover))) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid ROMX 0.2.0 writer arguments");
    }
    if (report != NULL) {
        supplied_report_size = report->struct_size;
        if (supplied_report_size < sizeof(*report)) return ROMX_E_INVALID_ARGUMENT;
        memset(report, 0, sizeof(*report)); report->struct_size = supplied_report_size;
    }
    states = (writer_entry_state_t *)calloc(entry_count, sizeof(*states));
    if (states == NULL) return ROMX_E_OUT_OF_MEMORY;
    for (index = 0U; index < entry_count; ++index) {
        uint32_t other;
        if (entries[index].struct_size < sizeof(entries[index]) ||
            entries[index].reserved != UINT16_C(0) ||
            (entries[index].flags & ~ROMX_RIDX_FLAGS_MASK) != UINT32_C(0) ||
            !valid_io(entries[index].source) ||
            !valid_virtual_path(entries[index].virtual_path) ||
            entries[index].format_id == UINT16_C(0xffff)) {
            result = romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "writer entry is invalid"); goto fail;
        }
        if ((entries[index].flags & ROMX_RIDX_ENTRYPOINT) != 0U) {
            if (entrypoint != UINT32_MAX || entries[index].format_id == ROMX_FORMAT_UNKNOWN) {
                result = romx_error_set(error, ROMX_E_INDEX, 0,
                    ROMX_OFFSET_UNKNOWN, "writer requires exactly one typed entrypoint"); goto fail;
            }
            entrypoint = index;
        }
        for (other = 0U; other < index; ++other) {
            if (ascii_fold_equal(entries[index].virtual_path,
                entries[other].virtual_path)) {
                result = romx_error_set(error, ROMX_E_VIRTUAL_PATH, 0,
                    ROMX_OFFSET_UNKNOWN, "writer virtual paths collide"); goto fail;
            }
        }
        result = entries[index].source->get_size(entries[index].source->user_data,
            &states[index].size, error);
        if (result != ROMX_OK) goto fail;
    }
    if (entrypoint == UINT32_MAX || states[entrypoint].size == UINT64_C(0)) {
        result = romx_error_set(error, ROMX_E_INDEX, 0,
            ROMX_OFFSET_UNKNOWN, "writer entrypoint is absent or empty"); goto fail;
    }

    if ((settings.flags & ROMX_WRITER_PROBE_PAYLOAD) != 0U &&
        (effective_metadata == NULL || effective_cover == NULL)) {
        romx_probe_info_t probe_info = ROMX_PROBE_INFO_INIT;
        result = romx_probe_open_io(entries[entrypoint].source,
            entries[entrypoint].format_id, &probe, error);
        if (result != ROMX_OK && result != ROMX_E_UNSUPPORTED) goto fail;
        if (result == ROMX_OK) {
            result = romx_probe_get_info(probe, &probe_info, error);
            if (result != ROMX_OK) goto fail;
            if (effective_metadata == NULL &&
                (probe_info.flags & ROMX_PROBE_HAS_NAME) != 0U) {
                uint64_t needed = UINT64_C(0);
                (void)romx_probe_copy_metadata_json(probe, NULL, 0U, &needed, NULL);
                probed_metadata = (uint8_t *)malloc((size_t)needed);
                if (probed_metadata == NULL) { result = ROMX_E_OUT_OF_MEMORY; goto fail; }
                result = romx_probe_copy_metadata_json(probe, probed_metadata,
                    needed, &needed, error);
                if (result != ROMX_OK) goto fail;
                effective_metadata = probed_metadata;
                effective_metadata_size = needed;
            }
            if (effective_cover == NULL &&
                (probe_info.flags & ROMX_PROBE_HAS_COVER) != 0U) {
                uint64_t needed = UINT64_C(0);
                (void)romx_probe_copy_cover_png(probe, NULL, 0U, &needed, NULL);
                probed_cover = (uint8_t *)malloc((size_t)needed);
                if (probed_cover == NULL) { result = ROMX_E_OUT_OF_MEMORY; goto fail; }
                result = romx_probe_copy_cover_png(probe, probed_cover,
                    needed, &needed, error);
                if (result != ROMX_OK) goto fail;
                cover_memory.bytes = probed_cover; cover_memory.size = needed;
                cover_memory_io.user_data = &cover_memory;
                cover_memory_io.get_size = memory_get_size;
                cover_memory_io.read_at = memory_read_at;
                effective_cover = &cover_memory_io;
            }
        }
    }
    if (effective_metadata_size > settings.max_metadata_size ||
        effective_metadata_size > (uint64_t)SIZE_MAX) {
        result = ROMX_E_METADATA_TOO_LARGE; goto fail;
    }
    if (effective_metadata != NULL) {
        result = romx_validate_metadata_bytes(effective_metadata,
            (size_t)effective_metadata_size, error);
        if (result != ROMX_OK) goto fail;
    }
    if (effective_cover != NULL) {
        uint8_t ignored_hash[32]; uint32_t width, height;
        result = effective_cover->get_size(effective_cover->user_data,
            &cover_size, error);
        if (result != ROMX_OK) goto fail;
        result = romx_validate_cover_io(effective_cover, cover_size,
            settings.max_cover_size, settings.max_cover_dimension,
            settings.io_chunk_size, ignored_hash, &width, &height, error);
        if (result != ROMX_OK) goto fail;
    }
    ridx_size = ROMX_RIDX_HEADER_SIZE + (uint64_t)entry_count * ROMX_RIDX_ENTRY_SIZE;
    if (ridx_size > (uint64_t)SIZE_MAX) { result = ROMX_E_RANGE; goto fail; }
    ridx = (uint8_t *)calloc(1U, (size_t)ridx_size);
    if (ridx == NULL) { result = ROMX_E_OUT_OF_MEMORY; goto fail; }
    result = create_temporary(destination, &temporary, &descriptor, error);
    if (result != ROMX_OK) goto fail;
    if ((settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U)
        romx_sha256_init(&immutable_context);

    for (index = 0U; index < entry_count; ++index) {
        uint32_t current = index == 0U ? entrypoint :
            (index <= entrypoint ? index - 1U : index);
        states[current].offset = offset;
        result = stream_entry(entries[current].source, states[current].size,
            descriptor, offset, settings.io_chunk_size,
            (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
                ? &immutable_context : NULL,
            (entries[current].flags & ROMX_RIDX_HAS_CRC32) != 0U
                ? &states[current].crc32 : NULL,
            error);
        if (result != ROMX_OK) goto fail;
        if (states[current].size > UINT64_MAX - offset) { result = ROMX_E_RANGE; goto fail; }
        offset += states[current].size;
    }
    payload_size = offset;

    memcpy(ridx, "RIDX", 4U);
    write_le16(ridx + 0x04U, UINT16_C(1));
    write_le16(ridx + 0x06U, UINT16_C(64));
    write_le32(ridx + 0x08U, entry_count);
    write_le32(ridx + 0x0CU, UINT32_C(512));
    for (index = 0U; index < entry_count; ++index) {
        uint8_t *stored = ridx + ROMX_RIDX_HEADER_SIZE +
            (size_t)index * ROMX_RIDX_ENTRY_SIZE;
        size_t path_size = strlen(entries[index].virtual_path);
        write_le32(stored + 0x00U, entries[index].flags);
        write_le16(stored + 0x04U, entries[index].format_id);
        write_le16(stored + 0x06U, (uint16_t)path_size);
        write_le64(stored + 0x08U, states[index].offset);
        write_le64(stored + 0x10U, states[index].size);
        if ((entries[index].flags & ROMX_RIDX_HAS_CRC32) != 0U)
            write_le32(stored + 0x18U, states[index].crc32);
        memcpy(stored + 0x20U, entries[index].virtual_path, path_size);
    }
    {
        uint32_t crc = romx_crc32_begin();
        crc = romx_crc32_update(crc, ridx, (size_t)ridx_size);
        crc = romx_crc32_finish(crc);
        write_le32(ridx + 0x14U, crc);
    }
    result = write_immutable(descriptor, ridx, (size_t)ridx_size,
        offset, (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
            ? &immutable_context : NULL, error);
    if (result != ROMX_OK) goto fail;
    offset += ridx_size;
    if (effective_metadata != NULL) {
        result = write_immutable(descriptor, effective_metadata,
            (size_t)effective_metadata_size, offset,
            (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
                ? &immutable_context : NULL, error);
        if (result != ROMX_OK) goto fail;
        offset += effective_metadata_size;
    }
    if (effective_cover != NULL) {
        result = stream_entry(effective_cover, cover_size, descriptor, offset,
            settings.io_chunk_size,
            (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
                ? &immutable_context : NULL,
            NULL, error);
        if (result != ROMX_OK) goto fail;
        offset += cover_size;
    }
    if (settings.mutable_capacity != UINT64_C(0)) {
        uint8_t mutable_header[4096];
        uint64_t aligned = (offset + UINT64_C(4095)) & ~UINT64_C(4095);
        immutable_padding = aligned - offset;
        result = write_zeroes(descriptor, immutable_padding, offset,
            (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
                ? &immutable_context : NULL, error);
        if (result != ROMX_OK) goto fail;
        offset = aligned;
        build_mutable_header(mutable_header, &settings);
        result = write_all(descriptor, mutable_header, sizeof(mutable_header),
            offset, error);
        if (result != ROMX_OK) goto fail;
        result = write_zeroes(descriptor,
            settings.mutable_capacity - sizeof(mutable_header),
            offset + sizeof(mutable_header), NULL, error);
        if (result != ROMX_OK) goto fail;
    }
    immutable_size = offset;
    if ((settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U)
        romx_sha256_finish(&immutable_context, immutable_hash);
    offset += settings.mutable_capacity;
    memset(footer, 0, sizeof(footer));
    memcpy(footer, "ROMX", 4U);
    write_le32(footer + 0x04U, ROMX_FORMAT_VERSION);
    write_le64(footer + 0x08U, payload_size);
    write_le64(footer + 0x10U, effective_metadata_size);
    write_le64(footer + 0x18U, cover_size);
    write_le64(footer + 0x20U, settings.mutable_capacity);
    write_le16(footer + 0x28U, settings.platform_id);
    write_le16(footer + 0x2AU, settings.launch_format_id);
    if ((settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U) {
        write_le32(footer + 0x2CU, ROMX_IMMUTABLE_HASH_SHA256);
        memcpy(footer + 0x30U, immutable_hash, 32U);
    }
    {
        uint32_t crc = romx_crc32_begin();
        crc = romx_crc32_update(crc, footer, sizeof(footer));
        crc = romx_crc32_finish(crc);
        write_le32(footer + 0x50U, crc);
    }
    result = write_all(descriptor, footer, sizeof(footer), offset, error);
    if (result != ROMX_OK) goto fail;
    file_size = offset + sizeof(footer);
    result = close_output(descriptor,
        (settings.flags & ROMX_WRITER_DURABLE) != 0U, error);
    descriptor = -1;
    if (result != ROMX_OK) goto fail;
    result = publish_temporary(temporary, destination,
        (settings.flags & ROMX_WRITER_REPLACE_EXISTING) != 0U, error);
    if (result != ROMX_OK) goto fail;
    if (report != NULL) {
        report->file_size = file_size;
        report->payload_size = payload_size;
        report->payload_index_size = ridx_size;
        report->metadata_size = effective_metadata_size;
        report->cover_size = cover_size;
        report->immutable_padding_size = immutable_padding;
        report->mutable_capacity = settings.mutable_capacity;
        report->entry_count = entry_count;
        report->immutable_hash_algorithm =
            (settings.flags & ROMX_WRITER_IMMUTABLE_SHA256) != 0U
                ? ROMX_IMMUTABLE_HASH_SHA256 : ROMX_IMMUTABLE_HASH_NONE;
        memcpy(report->immutable_sha256, immutable_hash, 32U);
    }
    free(temporary); free(states); free(ridx);
    free(probed_metadata); free(probed_cover); romx_probe_close(probe);
    romx_error_clear(error);
    (void)immutable_size;
    return ROMX_OK;

fail:
    if (descriptor >= 0) {
#if defined(_WIN32)
        _close(descriptor);
#else
        close(descriptor);
#endif
    }
    if (temporary != NULL) remove_utf8(temporary);
    free(temporary); free(states); free(ridx);
    free(probed_metadata); free(probed_cover); romx_probe_close(probe);
    return result;
}

static romx_result_t path_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    path_input_t *input = (path_input_t *)user;
    (void)error; *size = input->size; return ROMX_OK;
}

static romx_result_t path_read_at(void *user, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    path_input_t *input = (path_input_t *)user;
    size_t count;
#if defined(_WIN32)
    if (_fseeki64(input->file, (__int64)offset, SEEK_SET) != 0)
#else
    if (offset > (uint64_t)INT64_MAX || fseeko(input->file, (off_t)offset, SEEK_SET) != 0)
#endif
        return romx_error_set(error, ROMX_E_IO, errno, offset,
            "failed to seek writer input");
    count = fread(buffer, 1U, (size_t)size, input->file);
    *bytes_read = (uint64_t)count;
    if (count < (size_t)size && ferror(input->file))
        return romx_error_set(error, ROMX_E_IO, errno, offset + count,
            "failed to read writer input");
    return ROMX_OK;
}

static romx_result_t open_path_input(const char *path, path_input_t *input,
    romx_io_t *io, romx_error_t *error)
{
#if defined(_WIN32)
    wchar_t *wide = to_wide(path);
    __int64 end;
    if (wide == NULL || _wfopen_s(&input->file, wide, L"rb") != 0) input->file = NULL;
    free(wide);
    if (input->file == NULL || _fseeki64(input->file, 0, SEEK_END) != 0 ||
        (end = _ftelli64(input->file)) < 0 || _fseeki64(input->file, 0, SEEK_SET) != 0)
#else
    off_t end;
    input->file = fopen(path, "rb");
    if (input->file == NULL || fseeko(input->file, 0, SEEK_END) != 0 ||
        (end = ftello(input->file)) < 0 || fseeko(input->file, 0, SEEK_SET) != 0)
#endif
    {
        if (input->file != NULL) fclose(input->file);
        input->file = NULL;
        return romx_error_set(error, ROMX_E_IO, errno,
            ROMX_OFFSET_UNKNOWN, "failed to open writer input path");
    }
    input->size = (uint64_t)end;
    *io = (romx_io_t)ROMX_IO_INIT;
    io->user_data = input; io->get_size = path_get_size; io->read_at = path_read_at;
    return ROMX_OK;
}

romx_result_t romx_writer_write_path_entries(const char *destination,
    const romx_writer_path_entry_t *entries, uint32_t entry_count,
    const char *metadata_path, const char *cover_path,
    const romx_writer_options_t *options, romx_writer_report_t *report,
    romx_error_t *error)
{
    path_input_t *inputs = NULL;
    romx_io_t *ios = NULL;
    romx_writer_io_entry_t *io_entries = NULL;
    path_input_t cover_input = { NULL, UINT64_C(0) };
    romx_io_t cover_io = ROMX_IO_INIT;
    uint8_t *metadata = NULL;
    uint64_t metadata_size = UINT64_C(0);
    FILE *metadata_file = NULL;
    uint32_t index;
    romx_result_t result = ROMX_OK;
    if (destination == NULL || entries == NULL || entry_count == 0U) return ROMX_E_INVALID_ARGUMENT;
    inputs = (path_input_t *)calloc(entry_count, sizeof(*inputs));
    ios = (romx_io_t *)calloc(entry_count, sizeof(*ios));
    io_entries = (romx_writer_io_entry_t *)calloc(entry_count, sizeof(*io_entries));
    if (inputs == NULL || ios == NULL || io_entries == NULL) { result = ROMX_E_OUT_OF_MEMORY; goto done; }
    for (index = 0U; index < entry_count; ++index) {
        if (entries[index].struct_size < sizeof(entries[index]) ||
            entries[index].source_path == NULL) { result = ROMX_E_INVALID_ARGUMENT; goto done; }
        result = open_path_input(entries[index].source_path, &inputs[index], &ios[index], error);
        if (result != ROMX_OK) goto done;
        io_entries[index] = (romx_writer_io_entry_t)ROMX_WRITER_IO_ENTRY_INIT;
        io_entries[index].flags = entries[index].flags;
        io_entries[index].virtual_path = entries[index].virtual_path;
        io_entries[index].source = &ios[index];
        io_entries[index].format_id = entries[index].format_id;
    }
    if (metadata_path != NULL) {
        path_input_t temp = { NULL, UINT64_C(0) }; romx_io_t metadata_io = ROMX_IO_INIT;
        result = open_path_input(metadata_path, &temp, &metadata_io, error);
        if (result != ROMX_OK) goto done;
        metadata_file = temp.file; metadata_size = temp.size;
        if (metadata_size > (uint64_t)SIZE_MAX) { result = ROMX_E_METADATA_TOO_LARGE; goto done; }
        metadata = (uint8_t *)malloc((size_t)metadata_size);
        if (metadata == NULL) { result = ROMX_E_OUT_OF_MEMORY; goto done; }
        result = input_read_exact(&metadata_io, 0U, metadata, metadata_size, error);
        if (result != ROMX_OK) goto done;
    }
    if (cover_path != NULL) {
        result = open_path_input(cover_path, &cover_input, &cover_io, error);
        if (result != ROMX_OK) goto done;
    }
    result = romx_writer_write_io_entries(destination, io_entries, entry_count,
        metadata, metadata_size, cover_path != NULL ? &cover_io : NULL,
        options, report, error);
done:
    if (metadata_file != NULL) fclose(metadata_file);
    if (cover_input.file != NULL) fclose(cover_input.file);
    if (inputs != NULL) for (index = 0U; index < entry_count; ++index)
        if (inputs[index].file != NULL) fclose(inputs[index].file);
    free(metadata); free(io_entries); free(ios); free(inputs);
    return result;
}
