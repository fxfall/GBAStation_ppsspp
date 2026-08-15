#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define MUTABLE_STATE_ACTIVE UINT16_C(1)
#define MUTABLE_STATE_WRITING UINT16_C(2)
#define MUTABLE_STATE_DELETING UINT16_C(3)

typedef struct mutable_disk {
#if defined(_WIN32)
    HANDLE handle;
#else
    int descriptor;
#endif
    uint64_t size;
} mutable_disk_t;

typedef struct mutable_range {
    uint64_t start;
    uint64_t end;
} mutable_range_t;

typedef struct source_path {
    FILE *file;
    uint64_t size;
} source_path_t;

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value; bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index) bytes[index] = (uint8_t)(value >> (index * 8U));
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index) bytes[index] = (uint8_t)(value >> (index * 8U));
}

static int key_valid(romx_mutable_namespace_t object_namespace,
    const char *key)
{
    const uint8_t *bytes = (const uint8_t *)key;
    size_t size, index, component = 0U, bad = 0U;
    int private_separator = 0;
    if (key == NULL) return 0;
    size = strlen(key);
    if (size == 0U || size > ROMX_MUTABLE_KEY_CAPACITY ||
        bytes[0] == '/' || bytes[size - 1U] == '/' ||
        object_namespace < ROMX_MUTABLE_NAMESPACE_SAVE ||
        object_namespace > ROMX_MUTABLE_NAMESPACE_PRIVATE ||
        !romx_utf8_validate(bytes, size, &bad)) return 0;
    for (index = 0U; index <= size; ++index) {
        if (index < size && bytes[index] != '/') {
            if (bytes[index] == '\\') return 0;
            continue;
        }
        if (index == component ||
            (index - component == 1U && bytes[component] == '.') ||
            (index - component == 2U && bytes[component] == '.' && bytes[component + 1U] == '.')) return 0;
        if (object_namespace == ROMX_MUTABLE_NAMESPACE_PRIVATE &&
            component == 0U && index > 0U && index < size) private_separator = 1;
        component = index + 1U;
    }
    return object_namespace != ROMX_MUTABLE_NAMESPACE_PRIVATE || private_separator;
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

static romx_result_t disk_open_locked(const char *path, mutable_disk_t *disk,
    romx_error_t *error)
{
#if defined(_WIN32)
    LARGE_INTEGER size;
    OVERLAPPED lock;
    wchar_t *wide = to_wide(path);
    if (wide == NULL) return ROMX_E_INVALID_ARGUMENT;
    disk->handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
        FILE_FLAG_RANDOM_ACCESS, NULL);
    free(wide);
    memset(&lock, 0, sizeof(lock));
    if (disk->handle == INVALID_HANDLE_VALUE ||
        !LockFileEx(disk->handle, LOCKFILE_EXCLUSIVE_LOCK, 0,
            MAXDWORD, MAXDWORD, &lock) || !GetFileSizeEx(disk->handle, &size)) {
        DWORD code = GetLastError();
        if (disk->handle != INVALID_HANDLE_VALUE) CloseHandle(disk->handle);
        return romx_error_set(error, ROMX_E_IO, (int32_t)code,
            ROMX_OFFSET_UNKNOWN, "failed to open or lock mutable ROMX file");
    }
    disk->size = (uint64_t)size.QuadPart;
#else
    struct stat status;
    struct flock lock;
    disk->descriptor = open(path, O_RDWR);
    memset(&lock, 0, sizeof(lock)); lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
    if (disk->descriptor < 0 || fcntl(disk->descriptor, F_SETLKW, &lock) != 0 ||
        fstat(disk->descriptor, &status) != 0 || status.st_size < 0) {
        int code = errno;
        if (disk->descriptor >= 0) close(disk->descriptor);
        return romx_error_set(error, ROMX_E_IO, code,
            ROMX_OFFSET_UNKNOWN, "failed to open or lock mutable ROMX file");
    }
    disk->size = (uint64_t)status.st_size;
#endif
    return ROMX_OK;
}

static void disk_close(mutable_disk_t *disk)
{
#if defined(_WIN32)
    if (disk->handle != INVALID_HANDLE_VALUE) CloseHandle(disk->handle);
#else
    if (disk->descriptor >= 0) close(disk->descriptor);
#endif
}

static romx_result_t disk_read(void *user, uint64_t offset, void *buffer,
    uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    mutable_disk_t *disk = (mutable_disk_t *)user;
    *bytes_read = UINT64_C(0);
#if defined(_WIN32)
    while (*bytes_read < size) {
        LARGE_INTEGER position_value;
        DWORD actual = 0U;
        DWORD count = (DWORD)((size - *bytes_read) > UINT32_MAX ? UINT32_MAX : size - *bytes_read);
        uint64_t position = offset + *bytes_read;
        position_value.QuadPart = (__int64)position;
        if (!SetFilePointerEx(disk->handle, position_value, NULL, FILE_BEGIN))
            return romx_error_set(error, ROMX_E_IO,
                (int32_t)GetLastError(), position, "mutable seek failed");
        if (!ReadFile(disk->handle, (uint8_t *)buffer + (size_t)*bytes_read,
            count, &actual, NULL)) return romx_error_set(error, ROMX_E_IO,
                (int32_t)GetLastError(), position, "mutable read failed");
        *bytes_read += actual; if (actual != count) break;
    }
#else
    while (*bytes_read < size) {
        ssize_t count = pread(disk->descriptor,
            (uint8_t *)buffer + (size_t)*bytes_read,
            (size_t)(size - *bytes_read), (off_t)(offset + *bytes_read));
        if (count < 0) { if (errno == EINTR) continue; return romx_error_set(error,
            ROMX_E_IO, errno, offset + *bytes_read, "mutable read failed"); }
        if (count == 0) break; *bytes_read += (uint64_t)count;
    }
#endif
    return ROMX_OK;
}

static romx_result_t disk_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    mutable_disk_t *disk = (mutable_disk_t *)user;
    (void)error; *size = disk->size; return ROMX_OK;
}

static romx_result_t disk_write(mutable_disk_t *disk, uint64_t offset,
    const void *buffer, uint64_t size, romx_error_t *error)
{
    uint64_t written = UINT64_C(0);
#if defined(_WIN32)
    while (written < size) {
        LARGE_INTEGER position_value;
        DWORD actual = 0U;
        DWORD count = (DWORD)((size - written) > UINT32_MAX ? UINT32_MAX : size - written);
        uint64_t position = offset + written;
        position_value.QuadPart = (__int64)position;
        if (!SetFilePointerEx(disk->handle, position_value, NULL, FILE_BEGIN))
            return romx_error_set(error, ROMX_E_WRITE,
                (int32_t)GetLastError(), position, "mutable seek failed");
        if (!WriteFile(disk->handle, (const uint8_t *)buffer + (size_t)written,
            count, &actual, NULL) || actual != count)
            return romx_error_set(error, ROMX_E_WRITE,
                (int32_t)GetLastError(), position, "mutable write failed");
        written += actual;
    }
#else
    while (written < size) {
        ssize_t count = pwrite(disk->descriptor,
            (const uint8_t *)buffer + (size_t)written,
            (size_t)(size - written), (off_t)(offset + written));
        if (count < 0) { if (errno == EINTR) continue; return romx_error_set(error,
            ROMX_E_WRITE, errno, offset + written, "mutable write failed"); }
        if (count == 0) return ROMX_E_WRITE; written += (uint64_t)count;
    }
#endif
    return ROMX_OK;
}

static romx_result_t disk_sync(mutable_disk_t *disk, romx_error_t *error)
{
#if defined(_WIN32)
    if (!FlushFileBuffers(disk->handle)) return romx_error_set(error,
        ROMX_E_WRITE, (int32_t)GetLastError(), ROMX_OFFSET_UNKNOWN,
        "mutable commit sync failed");
#else
    if (fsync(disk->descriptor) != 0) return romx_error_set(error,
        ROMX_E_WRITE, errno, ROMX_OFFSET_UNKNOWN, "mutable commit sync failed");
#endif
    return ROMX_OK;
}

static int range_compare(const void *left, const void *right)
{
    const mutable_range_t *a = (const mutable_range_t *)left;
    const mutable_range_t *b = (const mutable_range_t *)right;
    return a->start < b->start ? -1 : (a->start > b->start ? 1 : 0);
}

static uint64_t align64(uint64_t value)
{
    if (value > UINT64_MAX - UINT64_C(63)) return UINT64_MAX;
    return (value + UINT64_C(63)) & ~UINT64_C(63);
}

static int find_free_extent(const romx_reader_t *reader, uint64_t capacity,
    uint64_t *offset)
{
    mutable_range_t *ranges;
    uint32_t count = 0U, index;
    uint64_t cursor = UINT64_C(4096) +
        (uint64_t)reader->mutable_slot_count * UINT64_C(512);
    ranges = (mutable_range_t *)calloc(reader->mutable_slot_count, sizeof(*ranges));
    if (ranges == NULL) return 0;
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (!slot->usable) continue;
        ranges[count].start = slot->object.data_offset;
        ranges[count].end = slot->object.data_offset + slot->object.data_capacity;
        ++count;
    }
    qsort(ranges, count, sizeof(*ranges), range_compare);
    for (index = 0U; index < count; ++index) {
        if (ranges[index].start >= cursor &&
            capacity <= ranges[index].start - cursor) break;
        if (ranges[index].end > cursor) cursor = align64(ranges[index].end);
    }
    free(ranges);
    if (cursor > reader->info.mutable_region.size ||
        capacity > reader->info.mutable_region.size - cursor) return 0;
    *offset = cursor; return 1;
}

static void build_entry(uint8_t stored[512], uint16_t state,
    romx_mutable_namespace_t object_namespace, const char *key,
    uint64_t data_offset, uint64_t data_capacity, uint64_t data_size,
    uint64_t generation, uint64_t modified, uint32_t data_crc)
{
    size_t key_size = strlen(key);
    uint32_t crc;
    memset(stored, 0, 512U); memcpy(stored, "MENT", 4U);
    write_le16(stored + 0x04U, state); write_le16(stored + 0x06U, object_namespace);
    write_le32(stored + 0x0CU, (uint32_t)key_size);
    write_le64(stored + 0x10U, data_offset); write_le64(stored + 0x18U, data_capacity);
    write_le64(stored + 0x20U, data_size); write_le64(stored + 0x28U, generation);
    write_le64(stored + 0x30U, modified); write_le32(stored + 0x38U, data_crc);
    memcpy(stored + 0x40U, key, key_size);
    crc = romx_crc32_begin(); crc = romx_crc32_update(crc, stored, 512U);
    crc = romx_crc32_finish(crc); write_le32(stored + 0x3CU, crc);
}

static romx_result_t source_crc(const romx_io_t *source, uint64_t size,
    uint32_t chunk_size, uint32_t *finished_crc, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    if (buffer == NULL) return ROMX_E_OUT_OF_MEMORY;
    while (position < size) {
        uint64_t wanted = size - position, count = UINT64_C(0);
        romx_result_t result;
        if (wanted > chunk_size) wanted = chunk_size;
        result = source->read_at(source->user_data, position, buffer, wanted,
            &count, error);
        if (result != ROMX_OK || count != wanted) { free(buffer); return result != ROMX_OK ? result : ROMX_E_TRUNCATED; }
        crc = romx_crc32_update(crc, buffer, (size_t)count); position += count;
    }
    free(buffer); *finished_crc = romx_crc32_finish(crc); return ROMX_OK;
}

static romx_result_t source_write(mutable_disk_t *disk, uint64_t target,
    const romx_io_t *source, uint64_t size, uint32_t chunk_size,
    uint32_t *finished_crc, romx_error_t *error)
{
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    if (buffer == NULL) return ROMX_E_OUT_OF_MEMORY;
    while (position < size) {
        uint64_t wanted = size - position, count = UINT64_C(0);
        romx_result_t result;
        if (wanted > chunk_size) wanted = chunk_size;
        result = source->read_at(source->user_data, position, buffer, wanted,
            &count, error);
        if (result != ROMX_OK || count != wanted) { free(buffer); return result != ROMX_OK ? result : ROMX_E_TRUNCATED; }
        result = disk_write(disk, target + position, buffer, count, error);
        if (result != ROMX_OK) { free(buffer); return result; }
        crc = romx_crc32_update(crc, buffer, (size_t)count); position += count;
    }
    free(buffer); *finished_crc = romx_crc32_finish(crc); return ROMX_OK;
}

romx_result_t romx_mutable_write_io_path(const char *path,
    romx_mutable_namespace_t object_namespace, const char *key,
    const romx_io_t *source, const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written, romx_error_t *error)
{
    mutable_disk_t disk;
    romx_io_t io = ROMX_IO_INIT;
    romx_reader_t *reader = NULL;
    const struct romx_mutable_slot *existing = NULL;
    uint32_t slot_index = UINT32_MAX, index, chunk_size = ROMX_DEFAULT_IO_CHUNK_SIZE;
    uint64_t source_size, data_offset, data_capacity, generation, modified = UINT64_C(0);
    uint32_t expected_crc, written_crc;
    uint8_t entry[512];
    romx_result_t result;
    memset(&disk, 0, sizeof(disk));
#if !defined(_WIN32)
    disk.descriptor = -1;
#endif
    if (path == NULL || source == NULL || source->struct_size < sizeof(*source) ||
        source->get_size == NULL || source->read_at == NULL ||
        !key_valid(object_namespace, key) ||
        (options != NULL && (options->struct_size < sizeof(*options) ||
            options->reserved != 0U || options->flags != UINT32_C(0))) ||
        (written != NULL && written->struct_size < sizeof(*written)))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable write arguments");
    if (options != NULL) {
        if (options->io_chunk_size != 0U) chunk_size = options->io_chunk_size;
        modified = options->modified_unix_seconds;
    }
    if (chunk_size < 1024U) return ROMX_E_INVALID_ARGUMENT;
    result = source->get_size(source->user_data, &source_size, error);
    if (result != ROMX_OK) return result;
    result = source_crc(source, source_size, chunk_size, &expected_crc, error);
    if (result != ROMX_OK) return result;
    result = disk_open_locked(path, &disk, error); if (result != ROMX_OK) return result;
    io.user_data = &disk; io.get_size = disk_get_size; io.read_at = disk_read;
    result = romx_reader_open_io(&io, NULL, &reader, error);
    if (result != ROMX_OK) goto done;
    if (reader->mutable_status == ROMX_MUTABLE_ABSENT) { result = ROMX_E_MUTABLE_ABSENT; goto done; }
    if (reader->mutable_status == ROMX_MUTABLE_INVALID) { result = ROMX_E_MUTABLE_HEADER; goto done; }
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (slot->usable && slot->state == MUTABLE_STATE_ACTIVE &&
            slot->object.object_namespace == object_namespace &&
            ascii_fold_equal(slot->object.key, key)) { existing = slot; slot_index = index; break; }
    }
    if (existing != NULL) {
        data_offset = existing->object.data_offset;
        data_capacity = existing->object.data_capacity;
        if (existing->object.generation == UINT64_MAX) {
            result = ROMX_E_RANGE; goto done;
        }
        generation = existing->object.generation + UINT64_C(1);
        if (source_size > data_capacity ||
            (options != NULL && options->data_capacity != 0U &&
                options->data_capacity != data_capacity)) { result = ROMX_E_MUTABLE_NO_SPACE; goto done; }
    } else {
        if (reader->mutable_status == ROMX_MUTABLE_DEGRADED) { result = ROMX_E_MUTABLE_NO_SPACE; goto done; }
        for (index = 0U; index < reader->mutable_slot_count; ++index) {
            if (reader->mutable_slots[index].object.struct_size == 0U) { slot_index = index; break; }
        }
        if (slot_index == UINT32_MAX) { result = ROMX_E_MUTABLE_NO_SPACE; goto done; }
        data_capacity = options != NULL && options->data_capacity != 0U
            ? align64(options->data_capacity) : align64(source_size == 0U ? 1U : source_size);
        if (data_capacity < source_size || !find_free_extent(reader, data_capacity, &data_offset)) {
            result = ROMX_E_MUTABLE_NO_SPACE; goto done;
        }
        generation = UINT64_C(1);
    }
    build_entry(entry, MUTABLE_STATE_WRITING, object_namespace, key,
        data_offset, data_capacity, source_size, generation, modified, expected_crc);
    result = disk_write(&disk, reader->info.mutable_region.offset + UINT64_C(4096) +
        (uint64_t)slot_index * UINT64_C(512), entry, sizeof(entry), error);
    if (result != ROMX_OK || (result = disk_sync(&disk, error)) != ROMX_OK) goto done;
    result = source_write(&disk, reader->info.mutable_region.offset + data_offset,
        source, source_size, chunk_size, &written_crc, error);
    if (result != ROMX_OK || written_crc != expected_crc) {
        if (result == ROMX_OK) result = ROMX_E_MUTABLE_DATA_CRC;
        goto done;
    }
    result = disk_sync(&disk, error); if (result != ROMX_OK) goto done;
    build_entry(entry, MUTABLE_STATE_ACTIVE, object_namespace, key,
        data_offset, data_capacity, source_size, generation, modified, expected_crc);
    result = disk_write(&disk, reader->info.mutable_region.offset + UINT64_C(4096) +
        (uint64_t)slot_index * UINT64_C(512), entry, sizeof(entry), error);
    if (result != ROMX_OK || (result = disk_sync(&disk, error)) != ROMX_OK) goto done;
    if (written != NULL) {
        uint32_t supplied = written->struct_size;
        memset(written, 0, sizeof(*written)); written->struct_size = supplied;
        written->slot_index = slot_index; written->object_namespace = object_namespace;
        written->data_offset = data_offset; written->data_capacity = data_capacity;
        written->data_size = source_size; written->generation = generation;
        written->modified_unix_seconds = modified; written->data_crc32 = expected_crc;
        written->key_size = (uint32_t)strlen(key); memcpy(written->key, key, written->key_size + 1U);
    }
    result = ROMX_OK;
done:
    romx_reader_close(reader); disk_close(&disk); return result;
}

static romx_result_t source_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    source_path_t *source = (source_path_t *)user; (void)error;
    *size = source->size; return ROMX_OK;
}

static romx_result_t source_read(void *user, uint64_t offset, void *buffer,
    uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    source_path_t *source = (source_path_t *)user;
#if defined(_WIN32)
    if (_fseeki64(source->file, (__int64)offset, SEEK_SET) != 0) return ROMX_E_IO;
#else
    if (offset > (uint64_t)INT64_MAX ||
        fseeko(source->file, (off_t)offset, SEEK_SET) != 0) return ROMX_E_IO;
#endif
    *bytes_read = fread(buffer, 1U, (size_t)size, source->file);
    return ferror(source->file) ? romx_error_set(error, ROMX_E_IO, errno,
        offset, "mutable source read failed") : ROMX_OK;
}

romx_result_t romx_mutable_write_path(const char *romx_path,
    romx_mutable_namespace_t object_namespace, const char *key,
    const char *source_path_value, const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written, romx_error_t *error)
{
    source_path_t source;
    romx_io_t io = ROMX_IO_INIT;
#if defined(_WIN32)
    __int64 end;
#else
    off_t end;
#endif
    romx_result_t result;
    if (source_path_value == NULL) return ROMX_E_INVALID_ARGUMENT;
#if defined(_WIN32)
    {
        wchar_t *wide = to_wide(source_path_value);
        source.file = NULL;
        if (wide != NULL) (void)_wfopen_s(&source.file, wide, L"rb");
        free(wide);
    }
#else
    source.file = fopen(source_path_value, "rb");
#endif
#if defined(_WIN32)
    if (source.file == NULL || _fseeki64(source.file, 0, SEEK_END) != 0 ||
        (end = _ftelli64(source.file)) < 0 || _fseeki64(source.file, 0, SEEK_SET) != 0) {
#else
    if (source.file == NULL || fseeko(source.file, 0, SEEK_END) != 0 ||
        (end = ftello(source.file)) < 0 || fseeko(source.file, 0, SEEK_SET) != 0) {
#endif
        if (source.file != NULL) fclose(source.file); return ROMX_E_IO;
    }
    source.size = (uint64_t)end;
    io.user_data = &source; io.get_size = source_get_size; io.read_at = source_read;
    result = romx_mutable_write_io_path(romx_path, object_namespace, key,
        &io, options, written, error);
    fclose(source.file); return result;
}

romx_result_t romx_mutable_delete_path(const char *path,
    romx_mutable_namespace_t object_namespace, const char *key,
    romx_error_t *error)
{
    mutable_disk_t disk;
    romx_io_t io = ROMX_IO_INIT;
    romx_reader_t *reader = NULL;
    const struct romx_mutable_slot *target = NULL;
    uint32_t index;
    uint8_t entry[512];
    uint8_t zero[512] = { 0 };
    uint64_t slot_offset;
    romx_result_t result;
    memset(&disk, 0, sizeof(disk));
#if !defined(_WIN32)
    disk.descriptor = -1;
#endif
    if (path == NULL || !key_valid(object_namespace, key)) return ROMX_E_INVALID_ARGUMENT;
    result = disk_open_locked(path, &disk, error); if (result != ROMX_OK) return result;
    io.user_data = &disk; io.get_size = disk_get_size; io.read_at = disk_read;
    result = romx_reader_open_io(&io, NULL, &reader, error); if (result != ROMX_OK) goto done;
    if (reader->mutable_status == ROMX_MUTABLE_ABSENT) {
        result = romx_error_set(error, ROMX_E_MUTABLE_ABSENT, 0,
            ROMX_OFFSET_UNKNOWN, "ROMX has no mutable region");
        goto done;
    }
    if (reader->mutable_status == ROMX_MUTABLE_INVALID) {
        result = romx_error_set(error, ROMX_E_MUTABLE_HEADER, 0,
            reader->info.mutable_region.offset, "ROMX mutable header is invalid");
        goto done;
    }
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (slot->usable && slot->state == MUTABLE_STATE_ACTIVE &&
            slot->object.object_namespace == object_namespace &&
            ascii_fold_equal(slot->object.key, key)) { target = slot; break; }
    }
    if (target == NULL) { result = ROMX_E_MUTABLE_ENTRY; goto done; }
    slot_offset = reader->info.mutable_region.offset + UINT64_C(4096) +
        (uint64_t)index * UINT64_C(512);
    build_entry(entry, MUTABLE_STATE_DELETING, object_namespace, key,
        target->object.data_offset, target->object.data_capacity,
        target->object.data_size, target->object.generation + UINT64_C(1),
        target->object.modified_unix_seconds, target->object.data_crc32);
    result = disk_write(&disk, slot_offset, entry, sizeof(entry), error);
    if (result != ROMX_OK || (result = disk_sync(&disk, error)) != ROMX_OK) goto done;
    result = disk_write(&disk, slot_offset, zero, sizeof(zero), error);
    if (result == ROMX_OK) result = disk_sync(&disk, error);
done:
    romx_reader_close(reader); disk_close(&disk); return result;
}
