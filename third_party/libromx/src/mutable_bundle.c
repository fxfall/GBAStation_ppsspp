#if !defined(_WIN32)
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define BUNDLE_HEADER_SIZE UINT64_C(64)
#define BUNDLE_ENTRY_SIZE UINT64_C(64)

#if defined(_WIN32)
static wchar_t *path_to_wide(const char *path)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            path, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static FILE *file_open_read(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    FILE *file = NULL;
    if (wide != NULL) (void)_wfopen_s(&file, wide, L"rb");
    free(wide);
    return file;
}
#else
static FILE *file_open_read(const char *path)
{
    return fopen(path, "rb");
}
#endif

typedef struct bundle_source_entry {
    char *path;
    FILE *source;
    uint64_t size;
    uint64_t data_offset;
    uint32_t crc32;
} bundle_source_entry_t;

typedef struct bundle_source {
    uint8_t *prefix;
    uint64_t prefix_size;
    uint64_t bundle_size;
    bundle_source_entry_t *entries;
    uint32_t entry_count;
} bundle_source_t;

typedef struct bundle_entry {
    char *path;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t data_crc32;
} bundle_entry_t;

struct romx_mutable_bundle {
    romx_mutable_file_t *file;
    bundle_entry_t *entries;
    uint32_t entry_count;
    uint32_t io_chunk_size;
};

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = UINT64_C(0);
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        value |= (uint64_t)bytes[index] << (index * 8U);
    return value;
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8U));
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8U));
}

static uint64_t align64(uint64_t value)
{
    if (value > UINT64_MAX - UINT64_C(63)) return UINT64_MAX;
    return (value + UINT64_C(63)) & ~UINT64_C(63);
}

static int bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;
    for (index = 0U; index < size; ++index)
        if (bytes[index] != 0U) return 0;
    return 1;
}

static uint32_t header_crc(uint8_t header[64])
{
    uint8_t saved[4];
    uint32_t crc;
    memcpy(saved, header + 0x38U, sizeof(saved));
    memset(header + 0x38U, 0, sizeof(saved));
    crc = romx_crc32_begin();
    crc = romx_crc32_update(crc, header, 64U);
    crc = romx_crc32_finish(crc);
    memcpy(header + 0x38U, saved, sizeof(saved));
    return crc;
}

static int path_valid(const char *path, uint32_t max_path)
{
    const uint8_t *bytes = (const uint8_t *)path;
    size_t size;
    size_t component = 0U;
    size_t index;
    size_t bad = 0U;
    if (path == NULL) return 0;
    size = strlen(path);
    if (size == 0U || size > max_path || bytes[0] == (uint8_t)'/' ||
        bytes[size - 1U] == (uint8_t)'/' ||
        !romx_utf8_validate(bytes, size, &bad)) return 0;
    for (index = 0U; index <= size; ++index) {
        if (index < size && bytes[index] != (uint8_t)'/') {
            if (bytes[index] == (uint8_t)'\\' || bytes[index] == UINT8_C(0))
                return 0;
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

static int path_compare_bytes(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (*a != 0U && *b != 0U) {
        if (*a != *b) return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return *a == *b ? 0 : (*a == 0U ? -1 : 1);
}

static int source_compare(const void *left, const void *right)
{
    const bundle_source_entry_t *a = (const bundle_source_entry_t *)left;
    const bundle_source_entry_t *b = (const bundle_source_entry_t *)right;
    return path_compare_bytes(a->path, b->path);
}

static int ascii_fold_equal(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
            a = (unsigned char)(a + 32U);
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z')
            b = (unsigned char)(b + 32U);
        if (a != b) return 0;
        if (a == 0U) return 1;
    }
}

static int ascii_fold_compare(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
            a = (unsigned char)(a + 32U);
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z')
            b = (unsigned char)(b + 32U);
        if (a != b) return a < b ? -1 : 1;
        if (a == 0U) return 0;
    }
}

static int folded_path_pointer_compare(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return ascii_fold_compare(*a, *b);
}

/* Returns 1 when unique, 0 for a collision, and -1 on allocation failure. */
static int paths_portable_unique(char *const *paths, uint32_t count)
{
    const char **sorted;
    uint32_t index;
    int unique = 1;
    if (count < UINT32_C(2)) return 1;
    sorted = (const char **)malloc((size_t)count * sizeof(*sorted));
    if (sorted == NULL) return -1;
    for (index = 0U; index < count; ++index) sorted[index] = paths[index];
    qsort(sorted, count, sizeof(*sorted), folded_path_pointer_compare);
    for (index = 1U; index < count; ++index) {
        if (ascii_fold_equal(sorted[index - 1U], sorted[index])) {
            unique = 0;
            break;
        }
    }
    free(sorted);
    return unique;
}

static void effective_options(const romx_mutable_bundle_options_t *provided,
    romx_mutable_bundle_options_t *options)
{
    *options = (romx_mutable_bundle_options_t)ROMX_MUTABLE_BUNDLE_OPTIONS_INIT;
    if (provided != NULL) *options = *provided;
    if (options->max_entry_count == UINT32_C(0))
        options->max_entry_count = ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_ENTRIES;
    if (options->max_path_size == UINT32_C(0))
        options->max_path_size = ROMX_MUTABLE_BUNDLE_PATH_CAPACITY;
    if (options->max_bundle_size == UINT64_C(0))
        options->max_bundle_size = ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_SIZE;
    if (options->io_chunk_size == UINT32_C(0))
        options->io_chunk_size = ROMX_DEFAULT_IO_CHUNK_SIZE;
}

static int file_seek(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    if (offset > (uint64_t)INT64_MAX) return 0;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static int file_get_size(FILE *file, uint64_t *size)
{
#if defined(_WIN32)
    __int64 value;
    if (_fseeki64(file, 0, SEEK_END) != 0 ||
        (value = _ftelli64(file)) < 0 || _fseeki64(file, 0, SEEK_SET) != 0)
        return 0;
#else
    off_t value;
    if (fseeko(file, 0, SEEK_END) != 0 ||
        (value = ftello(file)) < 0 || fseeko(file, 0, SEEK_SET) != 0)
        return 0;
#endif
    *size = (uint64_t)value;
    return 1;
}

static int source_path_is_regular(const char *path)
{
#if defined(_WIN32)
    wchar_t *wide = path_to_wide(path);
    DWORD attributes;
    if (wide == NULL) return 0;
    attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
            FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
#else
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode))
        return 0;
    return 1;
#endif
}

static romx_result_t file_crc(FILE *file, uint64_t size, uint32_t chunk_size,
    uint32_t *finished, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate bundle CRC buffer");
    if (!file_seek(file, UINT64_C(0))) {
        free(buffer);
        return romx_error_set(error, ROMX_E_IO, errno, ROMX_OFFSET_UNKNOWN,
            "failed to seek mutable bundle source");
    }
    while (position < size) {
        size_t wanted = (size - position) > chunk_size
            ? (size_t)chunk_size : (size_t)(size - position);
        size_t actual = fread(buffer, 1U, wanted, file);
        if (actual != wanted) {
            free(buffer);
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to read mutable bundle source");
        }
        crc = romx_crc32_update(crc, buffer, actual);
        position += (uint64_t)actual;
    }
    free(buffer);
    *finished = romx_crc32_finish(crc);
    return ROMX_OK;
}

static void source_destroy(bundle_source_t *source)
{
    uint32_t index;
    if (source == NULL) return;
    for (index = 0U; index < source->entry_count; ++index) {
        if (source->entries[index].source != NULL)
            fclose(source->entries[index].source);
        free(source->entries[index].path);
    }
    free(source->entries);
    free(source->prefix);
    memset(source, 0, sizeof(*source));
}

static romx_result_t source_get_size(void *user_data, uint64_t *size,
    romx_error_t *error)
{
    bundle_source_t *source = (bundle_source_t *)user_data;
    (void)error;
    *size = source->bundle_size;
    return ROMX_OK;
}

static romx_result_t source_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    bundle_source_t *source = (bundle_source_t *)user_data;
    uint8_t *output = (uint8_t *)buffer;
    uint64_t count;
    uint32_t index;
    if (offset > source->bundle_size)
        return romx_error_set(error, ROMX_E_RANGE, 0, offset,
            "mutable bundle read offset is out of range");
    count = source->bundle_size - offset;
    if (count > size) count = size;
    if (count > (uint64_t)SIZE_MAX)
        return romx_error_set(error, ROMX_E_RANGE, 0, offset,
            "mutable bundle read exceeds address space");
    memset(output, 0, (size_t)count);
    if (offset < source->prefix_size) {
        uint64_t prefix_count = source->prefix_size - offset;
        if (prefix_count > count) prefix_count = count;
        memcpy(output, source->prefix + (size_t)offset, (size_t)prefix_count);
    }
    for (index = 0U; index < source->entry_count; ++index) {
        bundle_source_entry_t *entry = &source->entries[index];
        uint64_t request_end = offset + count;
        uint64_t entry_end = entry->data_offset + entry->size;
        uint64_t start;
        uint64_t end;
        size_t wanted;
        if (request_end <= entry->data_offset || offset >= entry_end) continue;
        start = offset > entry->data_offset ? offset : entry->data_offset;
        end = request_end < entry_end ? request_end : entry_end;
        wanted = (size_t)(end - start);
        if (!file_seek(entry->source, start - entry->data_offset) ||
            fread(output + (size_t)(start - offset), 1U, wanted,
                entry->source) != wanted) {
            return romx_error_set(error, ROMX_E_IO, errno, start,
                "failed to read mutable bundle source");
        }
    }
    *bytes_read = count;
    romx_error_clear(error);
    return ROMX_OK;
}

static romx_result_t build_source(
    const romx_mutable_bundle_path_entry_t *provided, uint32_t entry_count,
    romx_mutable_namespace_t object_namespace,
    const romx_mutable_bundle_options_t *options, bundle_source_t *source,
    romx_error_t *error)
{
    uint64_t path_offset;
    uint64_t data_cursor;
    uint32_t index;
    memset(source, 0, sizeof(*source));
    source->entry_count = entry_count;
    if (entry_count != UINT32_C(0)) {
        source->entries = (bundle_source_entry_t *)calloc(entry_count,
            sizeof(*source->entries));
        if (source->entries == NULL)
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle entries");
    }
    for (index = 0U; index < entry_count; ++index) {
        const romx_mutable_bundle_path_entry_t *input = &provided[index];
        bundle_source_entry_t *entry = &source->entries[index];
        size_t path_size;
        romx_result_t result;
        if (input->struct_size < (uint32_t)sizeof(*input) ||
            input->reserved != UINT32_C(0) ||
            !path_valid(input->relative_path, options->max_path_size) ||
            input->source_path == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN, "invalid mutable bundle path entry");
        }
        path_size = strlen(input->relative_path);
        entry->path = (char *)malloc(path_size + 1U);
        if (entry->path == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to copy mutable bundle path");
        }
        memcpy(entry->path, input->relative_path, path_size + 1U);
        if (!source_path_is_regular(input->source_path)) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
                ROMX_OFFSET_UNKNOWN,
                "mutable bundle source is not a regular file");
        }
        entry->source = file_open_read(input->source_path);
        if (entry->source == NULL || !file_get_size(entry->source, &entry->size)) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_IO, errno,
                ROMX_OFFSET_UNKNOWN, "failed to open mutable bundle source");
        }
        result = file_crc(entry->source, entry->size, options->io_chunk_size,
            &entry->crc32, error);
        if (result != ROMX_OK) {
            source_destroy(source);
            return result;
        }
    }
    if (entry_count > UINT32_C(1))
        qsort(source->entries, entry_count, sizeof(*source->entries),
            source_compare);
    if (entry_count > UINT32_C(1)) {
        char **paths = (char **)malloc((size_t)entry_count * sizeof(*paths));
        int unique;
        if (paths == NULL) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to validate mutable bundle path uniqueness");
        }
        for (index = 0U; index < entry_count; ++index)
            paths[index] = source->entries[index].path;
        unique = paths_portable_unique(paths, entry_count);
        free(paths);
        if (unique <= 0) {
            source_destroy(source);
            return romx_error_set(error,
                unique < 0 ? ROMX_E_OUT_OF_MEMORY : ROMX_E_MUTABLE_BUNDLE,
                0, ROMX_OFFSET_UNKNOWN,
                unique < 0
                    ? "failed to validate mutable bundle path uniqueness"
                    : "mutable bundle paths are not portable-unique");
        }
    }

    path_offset = BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    if (path_offset > options->max_bundle_size) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle directory exceeds the limit");
    }
    for (index = 0U; index < entry_count; ++index) {
        size_t path_size = strlen(source->entries[index].path);
        if ((uint64_t)path_size > UINT64_MAX - path_offset) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle path table overflows");
        }
        path_offset += (uint64_t)path_size;
    }
    data_cursor = align64(path_offset);
    if (data_cursor == UINT64_MAX) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle alignment overflows");
    }
    for (index = 0U; index < entry_count; ++index) {
        source->entries[index].data_offset = data_cursor;
        if (source->entries[index].size > UINT64_MAX - data_cursor) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle data overflows");
        }
        data_cursor = align64(data_cursor + source->entries[index].size);
        if (data_cursor == UINT64_MAX) {
            source_destroy(source);
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable bundle alignment overflows");
        }
    }
    if (data_cursor > options->max_bundle_size ||
        data_cursor > (uint64_t)INT64_MAX ||
        data_cursor > (uint64_t)SIZE_MAX) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle exceeds the configured limit");
    }
    source->prefix_size = entry_count == UINT32_C(0)
        ? BUNDLE_HEADER_SIZE : source->entries[0].data_offset;
    source->bundle_size = data_cursor;
    source->prefix = (uint8_t *)calloc(1U, (size_t)source->prefix_size);
    if (source->prefix == NULL) {
        source_destroy(source);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle directory");
    }
    memcpy(source->prefix, "RMBL", 4U);
    write_le16(source->prefix + 0x04U, ROMX_MUTABLE_BUNDLE_VERSION);
    write_le16(source->prefix + 0x06U, UINT16_C(64));
    write_le16(source->prefix + 0x08U, object_namespace);
    write_le16(source->prefix + 0x0AU, UINT16_C(0));
    write_le32(source->prefix + 0x0CU, UINT32_C(64));
    write_le32(source->prefix + 0x10U, entry_count);
    write_le64(source->prefix + 0x18U, BUNDLE_HEADER_SIZE);
    write_le64(source->prefix + 0x20U, BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE);
    write_le64(source->prefix + 0x28U, source->prefix_size);
    write_le64(source->prefix + 0x30U, source->bundle_size);

    path_offset = BUNDLE_HEADER_SIZE +
        (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    for (index = 0U; index < entry_count; ++index) {
        uint8_t *stored = source->prefix + BUNDLE_HEADER_SIZE +
            (uint64_t)index * BUNDLE_ENTRY_SIZE;
        size_t path_size = strlen(source->entries[index].path);
        write_le64(stored + 0x00U, path_offset);
        write_le32(stored + 0x08U, (uint32_t)path_size);
        write_le64(stored + 0x10U, source->entries[index].data_offset);
        write_le64(stored + 0x18U, source->entries[index].size);
        write_le32(stored + 0x20U, source->entries[index].crc32);
        memcpy(source->prefix + (size_t)path_offset,
            source->entries[index].path, path_size);
        path_offset += (uint64_t)path_size;
    }
    write_le32(source->prefix + 0x38U, header_crc(source->prefix));
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_write_path_entries(const char *romx_path,
    romx_mutable_namespace_t object_namespace, const char *key,
    const romx_mutable_bundle_path_entry_t *entries, uint32_t entry_count,
    const romx_mutable_bundle_options_t *bundle_options,
    const romx_mutable_write_options_t *write_options,
    romx_mutable_object_info_t *written, romx_error_t *error)
{
    romx_mutable_bundle_options_t options;
    bundle_source_t source;
    romx_io_t io = ROMX_IO_INIT;
    romx_result_t result;
    effective_options(bundle_options, &options);
    if (romx_path == NULL || key == NULL ||
        (entries == NULL && entry_count != UINT32_C(0)) ||
        options.struct_size < (uint32_t)sizeof(options) ||
        options.flags != UINT32_C(0) || options.reserved != UINT32_C(0) ||
        options.max_entry_count == UINT32_C(0) ||
        options.max_path_size == UINT32_C(0) ||
        options.max_path_size > ROMX_MUTABLE_BUNDLE_PATH_CAPACITY ||
        options.io_chunk_size == UINT32_C(0) ||
        entry_count > options.max_entry_count ||
        (object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
         object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle write arguments");
    }
    result = build_source(entries, entry_count, object_namespace, &options,
        &source, error);
    if (result != ROMX_OK) return result;
    io.user_data = &source;
    io.get_size = source_get_size;
    io.read_at = source_read_at;
    result = romx_mutable_write_io_path(romx_path, object_namespace, key, &io,
        write_options, written, error);
    source_destroy(&source);
    return result;
}

static romx_result_t bundle_read_exact(romx_mutable_file_t *file,
    uint64_t offset, void *buffer, uint64_t size, romx_error_t *error)
{
    uint64_t position = UINT64_C(0);
    uint64_t read = UINT64_C(0);
    romx_result_t result = romx_mutable_file_seek(file, (int64_t)offset,
        ROMX_PAYLOAD_SEEK_START, &position, error);
    if (result != ROMX_OK) return result;
    result = romx_mutable_file_read(file, buffer, size, &read, error);
    if (result != ROMX_OK) return result;
    if (read != size) return romx_error_set(error, ROMX_E_TRUNCATED, 0,
        offset + read, "mutable bundle is truncated");
    return ROMX_OK;
}

static romx_result_t validate_zero_range(romx_mutable_file_t *file,
    uint64_t offset, uint64_t size, uint32_t chunk_size, romx_error_t *error)
{
    uint8_t *buffer;
    romx_result_t result = ROMX_OK;
    if (size == UINT64_C(0)) return ROMX_OK;
    buffer = (uint8_t *)malloc(chunk_size);
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        offset, "failed to allocate mutable bundle padding buffer");
    while (size != UINT64_C(0)) {
        uint64_t count = size > chunk_size ? chunk_size : size;
        result = bundle_read_exact(file, offset, buffer, count, error);
        if (result != ROMX_OK || !bytes_zero(buffer, (size_t)count)) {
            if (result == ROMX_OK)
                result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                    offset, "mutable bundle padding is not zero");
            break;
        }
        offset += count;
        size -= count;
    }
    free(buffer);
    return result;
}

static romx_result_t validate_entry_crc(romx_mutable_file_t *file,
    const bundle_entry_t *entry, uint32_t chunk_size, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    romx_result_t result = ROMX_OK;
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        entry->data_offset, "failed to allocate mutable bundle CRC buffer");
    while (position < entry->data_size) {
        uint64_t count = entry->data_size - position;
        if (count > chunk_size) count = chunk_size;
        result = bundle_read_exact(file, entry->data_offset + position,
            buffer, count, error);
        if (result != ROMX_OK) break;
        crc = romx_crc32_update(crc, buffer, (size_t)count);
        position += count;
    }
    free(buffer);
    if (result != ROMX_OK) return result;
    if (romx_crc32_finish(crc) != entry->data_crc32)
        return romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            entry->data_offset, "mutable bundle entry CRC32 mismatch");
    return ROMX_OK;
}

void romx_mutable_bundle_close(romx_mutable_bundle_t *bundle)
{
    uint32_t index;
    if (bundle == NULL) return;
    for (index = 0U; index < bundle->entry_count; ++index)
        free(bundle->entries[index].path);
    free(bundle->entries);
    romx_mutable_file_close(bundle->file);
    free(bundle);
}

romx_result_t romx_mutable_bundle_open(const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace, const char *key,
    const romx_mutable_bundle_options_t *provided,
    romx_mutable_bundle_t **out_bundle, romx_error_t *error)
{
    romx_mutable_bundle_options_t options;
    romx_mutable_bundle_t *bundle = NULL;
    uint8_t header[64];
    uint8_t *directory = NULL;
    uint8_t *paths = NULL;
    uint64_t object_size = UINT64_C(0);
    uint64_t directory_size;
    uint64_t path_table_offset;
    uint64_t data_offset;
    uint64_t bundle_size;
    uint64_t path_cursor;
    uint64_t data_cursor;
    uint32_t entry_count;
    uint32_t index;
    romx_result_t result;

    effective_options(provided, &options);
    if (reader == NULL || key == NULL || out_bundle == NULL ||
        options.struct_size < (uint32_t)sizeof(options) ||
        options.flags != UINT32_C(0) || options.reserved != UINT32_C(0) ||
        options.max_entry_count == UINT32_C(0) ||
        options.max_path_size == UINT32_C(0) ||
        options.max_path_size > ROMX_MUTABLE_BUNDLE_PATH_CAPACITY ||
        options.max_bundle_size < BUNDLE_HEADER_SIZE ||
        options.io_chunk_size == UINT32_C(0) ||
        (object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE &&
         object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle open arguments");
    }
    *out_bundle = NULL;
    bundle = (romx_mutable_bundle_t *)calloc(1U, sizeof(*bundle));
    if (bundle == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle");
    result = romx_mutable_file_open(reader, object_namespace, key,
        &bundle->file, error);
    if (result != ROMX_OK) goto fail;
    result = romx_mutable_file_get_size(bundle->file, &object_size, error);
    if (result != ROMX_OK) goto fail;
    if (object_size < BUNDLE_HEADER_SIZE ||
        object_size > (uint64_t)INT64_MAX ||
        object_size > options.max_bundle_size) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle size is outside the limit");
        goto fail;
    }
    result = bundle_read_exact(bundle->file, UINT64_C(0), header,
        sizeof(header), error);
    if (result != ROMX_OK) goto fail;
    entry_count = read_le32(header + 0x10U);
    directory_size = (uint64_t)entry_count * BUNDLE_ENTRY_SIZE;
    path_table_offset = read_le64(header + 0x20U);
    data_offset = read_le64(header + 0x28U);
    bundle_size = read_le64(header + 0x30U);
    if (memcmp(header, "RMBL", 4U) != 0 ||
        read_le16(header + 0x04U) != ROMX_MUTABLE_BUNDLE_VERSION ||
        read_le16(header + 0x06U) != UINT16_C(64) ||
        read_le16(header + 0x08U) != object_namespace ||
        read_le16(header + 0x0AU) != UINT16_C(0) ||
        read_le32(header + 0x0CU) != UINT32_C(64) ||
        entry_count > options.max_entry_count ||
        read_le32(header + 0x14U) != UINT32_C(0) ||
        read_le64(header + 0x18U) != BUNDLE_HEADER_SIZE ||
        path_table_offset != BUNDLE_HEADER_SIZE + directory_size ||
        data_offset < path_table_offset || data_offset % UINT64_C(64) != 0U ||
        bundle_size != object_size || data_offset > bundle_size ||
        read_le32(header + 0x38U) != header_crc(header) ||
        !bytes_zero(header + 0x3CU, 4U)) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle header is invalid");
        goto fail;
    }
    if (directory_size > (uint64_t)SIZE_MAX ||
        data_offset - path_table_offset > (uint64_t)SIZE_MAX) {
        result = romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle directory exceeds address space");
        goto fail;
    }
    if (directory_size != UINT64_C(0)) {
        directory = (uint8_t *)malloc((size_t)directory_size);
        bundle->entries = (bundle_entry_t *)calloc(entry_count,
            sizeof(*bundle->entries));
        if (directory == NULL || bundle->entries == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to allocate mutable bundle directory");
            goto fail;
        }
        result = bundle_read_exact(bundle->file, BUNDLE_HEADER_SIZE,
            directory, directory_size, error);
        if (result != ROMX_OK) goto fail;
    }
    if (data_offset != path_table_offset) {
        paths = (uint8_t *)malloc((size_t)(data_offset - path_table_offset));
        if (paths == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN, "failed to allocate mutable bundle paths");
            goto fail;
        }
        result = bundle_read_exact(bundle->file, path_table_offset, paths,
            data_offset - path_table_offset, error);
        if (result != ROMX_OK) goto fail;
    }
    bundle->entry_count = entry_count;
    bundle->io_chunk_size = options.io_chunk_size;
    path_cursor = path_table_offset;
    data_cursor = data_offset;
    for (index = 0U; index < entry_count; ++index) {
        const uint8_t *stored = directory + (uint64_t)index * BUNDLE_ENTRY_SIZE;
        bundle_entry_t *entry = &bundle->entries[index];
        uint64_t stored_path_offset = read_le64(stored + 0x00U);
        uint32_t path_size = read_le32(stored + 0x08U);
        uint64_t stored_data_offset = read_le64(stored + 0x10U);
        uint64_t data_size = read_le64(stored + 0x18U);
        if (stored_path_offset != path_cursor || path_size == UINT32_C(0) ||
            path_size > options.max_path_size ||
            path_cursor > data_offset ||
            path_size > data_offset - path_cursor ||
            read_le32(stored + 0x0CU) != UINT32_C(0) ||
            stored_data_offset != data_cursor ||
            data_size > bundle_size - stored_data_offset ||
            !bytes_zero(stored + 0x24U, 28U)) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                BUNDLE_HEADER_SIZE + (uint64_t)index * BUNDLE_ENTRY_SIZE,
                "mutable bundle entry is invalid");
            goto fail;
        }
        entry->path = (char *)malloc((size_t)path_size + 1U);
        if (entry->path == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                stored_path_offset, "failed to allocate mutable bundle path");
            goto fail;
        }
        memcpy(entry->path, paths + (size_t)(path_cursor - path_table_offset),
            path_size);
        entry->path[path_size] = '\0';
        if (!path_valid(entry->path, options.max_path_size) ||
            (index != UINT32_C(0) &&
             (path_compare_bytes(bundle->entries[index - 1U].path,
                    entry->path) >= 0 ||
              ascii_fold_equal(bundle->entries[index - 1U].path,
                    entry->path)))) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                stored_path_offset,
                "mutable bundle paths are invalid or not canonically ordered");
            goto fail;
        }
        entry->data_offset = stored_data_offset;
        entry->data_size = data_size;
        entry->data_crc32 = read_le32(stored + 0x20U);
        path_cursor += path_size;
        data_cursor = align64(stored_data_offset + data_size);
        if (data_cursor == UINT64_MAX || data_cursor > bundle_size) {
            result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
                stored_data_offset, "mutable bundle entry range overflows");
            goto fail;
        }
    }
    if (entry_count > UINT32_C(1)) {
        char **entry_paths = (char **)malloc(
            (size_t)entry_count * sizeof(*entry_paths));
        int unique;
        if (entry_paths == NULL) {
            result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
                ROMX_OFFSET_UNKNOWN,
                "failed to validate mutable bundle path uniqueness");
            goto fail;
        }
        for (index = 0U; index < entry_count; ++index)
            entry_paths[index] = bundle->entries[index].path;
        unique = paths_portable_unique(entry_paths, entry_count);
        free(entry_paths);
        if (unique <= 0) {
            result = romx_error_set(error,
                unique < 0 ? ROMX_E_OUT_OF_MEMORY : ROMX_E_MUTABLE_BUNDLE,
                0, ROMX_OFFSET_UNKNOWN,
                unique < 0
                    ? "failed to validate mutable bundle path uniqueness"
                    : "mutable bundle paths are not portable-unique");
            goto fail;
        }
    }
    if (align64(path_cursor) != data_offset ||
        data_cursor != bundle_size) {
        result = romx_error_set(error, ROMX_E_MUTABLE_BUNDLE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable bundle has non-canonical layout");
        goto fail;
    }
    result = validate_zero_range(bundle->file, path_cursor,
        data_offset - path_cursor, options.io_chunk_size, error);
    if (result != ROMX_OK) goto fail;
    for (index = 0U; index < entry_count; ++index) {
        const bundle_entry_t *entry = &bundle->entries[index];
        uint64_t end = entry->data_offset + entry->data_size;
        uint64_t aligned_end = align64(end);
        result = validate_entry_crc(bundle->file, entry,
            options.io_chunk_size, error);
        if (result != ROMX_OK) goto fail;
        result = validate_zero_range(bundle->file, end, aligned_end - end,
            options.io_chunk_size, error);
        if (result != ROMX_OK) goto fail;
    }
    free(directory);
    free(paths);
    *out_bundle = bundle;
    romx_error_clear(error);
    return ROMX_OK;

fail:
    free(directory);
    free(paths);
    romx_mutable_bundle_close(bundle);
    return result;
}

romx_result_t romx_mutable_bundle_get_entry_count(
    const romx_mutable_bundle_t *bundle, uint32_t *count, romx_error_t *error)
{
    if (bundle == NULL || count == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle count arguments");
    *count = bundle->entry_count;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_get_entry(
    const romx_mutable_bundle_t *bundle, uint32_t index,
    romx_mutable_bundle_entry_info_t *entry, romx_error_t *error)
{
    const bundle_entry_t *stored;
    size_t path_size;
    if (bundle == NULL || entry == NULL ||
        entry->struct_size < (uint32_t)sizeof(*entry) ||
        index >= bundle->entry_count)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle entry arguments");
    stored = &bundle->entries[index];
    path_size = strlen(stored->path);
    memset(entry, 0, sizeof(*entry));
    entry->struct_size = (uint32_t)sizeof(*entry);
    entry->index = index;
    entry->data_size = stored->data_size;
    entry->data_crc32 = stored->data_crc32;
    entry->path_size = (uint32_t)path_size;
    memcpy(entry->path, stored->path, path_size + 1U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_mutable_bundle_read_entry(romx_mutable_bundle_t *bundle,
    uint32_t index, uint64_t entry_offset, void *buffer,
    uint64_t buffer_size, uint64_t *bytes_read, romx_error_t *error)
{
    const bundle_entry_t *entry;
    uint64_t count;
    uint64_t position = UINT64_C(0);
    romx_result_t result;
    if (bundle == NULL || index >= bundle->entry_count ||
        bytes_read == NULL || (buffer == NULL && buffer_size != UINT64_C(0)))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable bundle read arguments");
    entry = &bundle->entries[index];
    if (entry_offset > entry->data_size)
        return romx_error_set(error, ROMX_E_RANGE, 0, entry_offset,
            "mutable bundle entry offset is out of range");
    count = entry->data_size - entry_offset;
    if (count > buffer_size) count = buffer_size;
    result = romx_mutable_file_seek(bundle->file,
        (int64_t)(entry->data_offset + entry_offset),
        ROMX_PAYLOAD_SEEK_START, &position, error);
    if (result != ROMX_OK) return result;
    result = romx_mutable_file_read(bundle->file, buffer, count,
        bytes_read, error);
    if (result == ROMX_OK && *bytes_read != count)
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            entry->data_offset + entry_offset + *bytes_read,
            "mutable bundle entry is truncated");
    return result;
}
