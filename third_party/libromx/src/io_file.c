#if !defined(_WIN32)
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

typedef struct romx_file_io {
    HANDLE handle;
    uint64_t size;
    CRITICAL_SECTION lock;
    int lock_initialized;
} romx_file_io_t;

static wchar_t *romx_utf8_to_wide(const char *path)
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

static void romx_file_close(void *user_data)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    if (state != NULL) {
        if (state->lock_initialized) DeleteCriticalSection(&state->lock);
        if (state->handle != INVALID_HANDLE_VALUE) CloseHandle(state->handle);
        free(state);
    }
}

static romx_result_t romx_file_get_size(void *user_data, uint64_t *size, romx_error_t *error)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    if (state == NULL || size == NULL) return romx_error_set(error,
        ROMX_E_INVALID_ARGUMENT, 0, ROMX_OFFSET_UNKNOWN, "invalid file I/O state");
    *size = state->size;
    return ROMX_OK;
}

static romx_result_t romx_file_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    uint8_t *output = (uint8_t *)buffer;
    *bytes_read = 0U;
    while (*bytes_read < size) {
        LARGE_INTEGER position;
        DWORD actual = 0U;
        DWORD count = (DWORD)((size - *bytes_read) > UINT32_MAX ? UINT32_MAX : size - *bytes_read);
        uint64_t byte_offset = offset + *bytes_read;
        DWORD code = ERROR_SUCCESS;

        position.QuadPart = (LONGLONG)byte_offset;
        EnterCriticalSection(&state->lock);
        if (!SetFilePointerEx(state->handle, position, NULL, FILE_BEGIN)) {
            code = GetLastError();
        } else if (!ReadFile(state->handle,
                output + (size_t)*bytes_read, count, &actual, NULL)) {
            code = GetLastError();
        }
        LeaveCriticalSection(&state->lock);
        if (code != ERROR_SUCCESS) {
            if (code == ERROR_HANDLE_EOF) {
                return ROMX_OK;
            }
            return romx_error_set(error, ROMX_E_IO,
                (int32_t)code, byte_offset, "failed to read ROMX file");
        }
        *bytes_read += actual;
        if (actual != count) break;
    }
    return ROMX_OK;
}

#else

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

typedef struct romx_file_io { int descriptor; uint64_t size; } romx_file_io_t;

static int romx_file_pread_exact(
    int descriptor, uint64_t offset, uint8_t *buffer, size_t size,
    size_t *bytes_read, int *system_code)
{
    *bytes_read = 0U;
    *system_code = 0;
    while (*bytes_read < size) {
        ssize_t actual = pread(descriptor, buffer + *bytes_read,
            size - *bytes_read, (off_t)(offset + (uint64_t)*bytes_read));
        if (actual < 0) {
            if (errno == EINTR) continue;
            *system_code = errno;
            return 0;
        }
        if (actual == 0) break;
        *bytes_read += (size_t)actual;
    }
    return *bytes_read == size;
}

static void romx_file_mapping_release(romx_payload_mapping_t *mapping)
{
    if (mapping->allocation != NULL && mapping->allocation_size != 0U) {
        (void)munmap(mapping->allocation, mapping->allocation_size);
    }
}

static romx_result_t romx_file_map_payload(
    void *user_data, romx_region_info_t region,
    romx_payload_mapping_t **out_mapping, romx_error_t *error)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    romx_payload_mapping_t *mapping = NULL;
    long page_query;
    size_t page_size;
    uint64_t aligned_offset;
    uint64_t delta;
    uint64_t span64;
    size_t span;
    size_t allocation_size;
    uint8_t *allocation;
    uint8_t *target;
    uint64_t payload_end;
    uint64_t middle_start;
    uint64_t middle_end;

    if (state == NULL || out_mapping == NULL || region.size == UINT64_C(0) ||
        region.offset > UINT64_MAX - region.size ||
        region.size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_RANGE, 0, region.offset,
            "payload cannot be represented as a memory mapping");
    }
    page_query = sysconf(_SC_PAGESIZE);
    if (page_query <= 0) {
        return romx_error_set(error, ROMX_E_UNSUPPORTED, errno,
            ROMX_OFFSET_UNKNOWN, "platform page size is unavailable");
    }
    page_size = (size_t)page_query;
    aligned_offset = region.offset - (region.offset % (uint64_t)page_size);
    delta = region.offset - aligned_offset;
    if (delta > UINT64_MAX - region.size) {
        return romx_error_set(error, ROMX_E_RANGE, 0, region.offset,
            "payload mapping span overflows");
    }
    span64 = delta + region.size;
    if (span64 > UINT64_MAX - ((uint64_t)page_size - UINT64_C(1))) {
        return romx_error_set(error, ROMX_E_RANGE, 0, region.offset,
            "payload mapping alignment overflows");
    }
    span64 = ((span64 + (uint64_t)page_size - UINT64_C(1)) /
        (uint64_t)page_size) * (uint64_t)page_size;
    if (span64 > (uint64_t)SIZE_MAX ||
        (size_t)span64 > SIZE_MAX - (page_size * 2U)) {
        return romx_error_set(error, ROMX_E_RANGE, 0, region.offset,
            "payload mapping exceeds address space");
    }
    span = (size_t)span64;
    allocation_size = span + page_size * 2U;
    allocation = (uint8_t *)mmap(NULL, allocation_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (allocation == MAP_FAILED) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, errno,
            ROMX_OFFSET_UNKNOWN, "failed to reserve guarded payload address space");
    }
    target = allocation + page_size;
    payload_end = region.offset + region.size;
    middle_start = region.offset;
    if ((middle_start % (uint64_t)page_size) != UINT64_C(0)) {
        middle_start += (uint64_t)page_size -
            (middle_start % (uint64_t)page_size);
    }
    if (middle_start > payload_end) {
        middle_start = payload_end;
    }
    middle_end = payload_end - (payload_end % (uint64_t)page_size);
    if (middle_end < middle_start) {
        middle_end = middle_start;
    }

    if (middle_end > middle_start) {
        uint64_t target_offset64 = middle_start - aligned_offset;
        size_t target_offset = (size_t)target_offset64;
        size_t middle_size = (size_t)(middle_end - middle_start);
        void *view = mmap(target + target_offset, middle_size, PROT_READ,
            MAP_PRIVATE | MAP_FIXED, state->descriptor, (off_t)middle_start);
        if (view == MAP_FAILED) {
            int code = errno;
            (void)munmap(allocation, allocation_size);
            return romx_error_set(error, ROMX_E_IO, code, middle_start,
                "failed to map payload file pages");
        }
    }

    /* Copy only partial boundary pages. Their non-payload bytes remain zero,
     * so adjacent metadata/cover/footer bytes cannot be observed. */
    {
        uint64_t first_page_end = aligned_offset + (uint64_t)page_size;
        uint64_t first_end = payload_end < first_page_end ? payload_end : first_page_end;
        if (region.offset != aligned_offset || payload_end < first_page_end) {
            size_t count = (size_t)(first_end - region.offset);
            size_t actual;
            int map_code;
            void *page = mmap(target, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            if (page == MAP_FAILED) {
                int code = errno;
                (void)munmap(allocation, allocation_size);
                return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, code,
                    region.offset, "failed to allocate payload boundary page");
            }
            if (!romx_file_pread_exact(state->descriptor, region.offset,
                    target + (size_t)delta, count, &actual, &map_code)) {
                (void)munmap(allocation, allocation_size);
                return romx_error_set(error, map_code != 0 ? ROMX_E_IO : ROMX_E_TRUNCATED,
                    map_code, region.offset, "failed to populate payload boundary page");
            }
            if (mprotect(target, page_size, PROT_READ) != 0) {
                int code = errno;
                (void)munmap(allocation, allocation_size);
                return romx_error_set(error, ROMX_E_IO, code, region.offset,
                    "failed to protect payload boundary page");
            }
        }
    }
    if ((payload_end % (uint64_t)page_size) != UINT64_C(0) &&
        (payload_end - UINT64_C(1)) / (uint64_t)page_size !=
        region.offset / (uint64_t)page_size) {
        uint64_t last_start = payload_end -
            (payload_end % (uint64_t)page_size);
        size_t target_offset = (size_t)(last_start - aligned_offset);
        size_t count = (size_t)(payload_end - last_start);
        size_t actual;
        int map_code;
        void *page = mmap(target + target_offset, page_size,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (page == MAP_FAILED) {
            int code = errno;
            (void)munmap(allocation, allocation_size);
            return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, code,
                last_start, "failed to allocate payload boundary page");
        }
        if (!romx_file_pread_exact(state->descriptor, last_start,
                target + target_offset, count, &actual, &map_code)) {
            (void)munmap(allocation, allocation_size);
            return romx_error_set(error, map_code != 0 ? ROMX_E_IO : ROMX_E_TRUNCATED,
                map_code, last_start, "failed to populate payload boundary page");
        }
        if (mprotect(target + target_offset, page_size, PROT_READ) != 0) {
            int code = errno;
            (void)munmap(allocation, allocation_size);
            return romx_error_set(error, ROMX_E_IO, code, last_start,
                "failed to protect payload boundary page");
        }
    }

    mapping = (romx_payload_mapping_t *)calloc(1U, sizeof(*mapping));
    if (mapping == NULL) {
        (void)munmap(allocation, allocation_size);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate payload mapping handle");
    }
    mapping->data = target + (size_t)delta;
    mapping->size = region.size;
    mapping->allocation = allocation;
    mapping->allocation_size = allocation_size;
    mapping->release = romx_file_mapping_release;
    *out_mapping = mapping;
    romx_error_clear(error);
    return ROMX_OK;
}

static void romx_file_close(void *user_data)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    if (state != NULL) { if (state->descriptor >= 0) (void)close(state->descriptor); free(state); }
}

static romx_result_t romx_file_get_size(void *user_data, uint64_t *size, romx_error_t *error)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    if (state == NULL || size == NULL) return romx_error_set(error,
        ROMX_E_INVALID_ARGUMENT, 0, ROMX_OFFSET_UNKNOWN, "invalid file I/O state");
    *size = state->size;
    return ROMX_OK;
}

static romx_result_t romx_file_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    romx_file_io_t *state = (romx_file_io_t *)user_data;
    uint8_t *output = (uint8_t *)buffer;
    *bytes_read = UINT64_C(0);
    if (offset > (uint64_t)INT64_MAX) return romx_error_set(error,
        ROMX_E_RANGE, 0, offset, "file offset exceeds platform limit");
    while (*bytes_read < size) {
        uint64_t remaining = size - *bytes_read;
        size_t count = remaining > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)remaining;
        ssize_t actual = pread(state->descriptor, output + (size_t)*bytes_read,
            count, (off_t)(offset + *bytes_read));
        if (actual < 0) {
            if (errno == EINTR) continue;
            return romx_error_set(error, ROMX_E_IO, errno, offset + *bytes_read,
                "failed to read ROMX file");
        }
        if (actual == 0) break;
        *bytes_read += (uint64_t)actual;
    }
    return ROMX_OK;
}
#endif

romx_result_t romx_reader_open_path(const char *utf8_path,
    const romx_reader_options_t *options, romx_reader_t **out_reader,
    romx_error_t *error)
{
    romx_file_io_t *state;
    romx_io_t io = ROMX_IO_INIT;
    romx_result_t result;
    romx_error_clear(error);
    if (out_reader != NULL) *out_reader = NULL;
    if (utf8_path == NULL || utf8_path[0] == '\0' || out_reader == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "path and out_reader must not be null or empty");
    state = (romx_file_io_t *)calloc(1U, sizeof(*state));
    if (state == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate file I/O state");
#if defined(_WIN32)
    {
        LARGE_INTEGER size;
        wchar_t *wide = romx_utf8_to_wide(utf8_path);
        if (wide == NULL) { free(state); return romx_error_set(error,
            ROMX_E_INVALID_ARGUMENT, (int32_t)GetLastError(), ROMX_OFFSET_UNKNOWN,
            "path is not valid UTF-8"); }
        state->handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
        free(wide);
        if (state->handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(state->handle, &size)) {
            DWORD code = GetLastError(); romx_file_close(state);
            return romx_error_set(error, ROMX_E_IO, (int32_t)code,
                ROMX_OFFSET_UNKNOWN, "failed to open or size ROMX file");
        }
        state->size = (uint64_t)size.QuadPart;
        InitializeCriticalSection(&state->lock);
        state->lock_initialized = 1;
    }
#else
    {
        struct stat status;
        state->descriptor = open(utf8_path, O_RDONLY);
        if (state->descriptor < 0 || fstat(state->descriptor, &status) != 0 || status.st_size < 0) {
            int code = errno; romx_file_close(state);
            return romx_error_set(error, ROMX_E_IO, code,
                ROMX_OFFSET_UNKNOWN, "failed to open or size ROMX file");
        }
        state->size = (uint64_t)status.st_size;
    }
#endif
    io.user_data = state; io.get_size = romx_file_get_size; io.read_at = romx_file_read_at;
    result = romx_reader_create(&io, options, romx_file_close, out_reader, error);
    if (result != ROMX_OK) romx_file_close(state);
#if !defined(_WIN32)
    else (*out_reader)->map_payload = romx_file_map_payload;
#endif
    return result;
}
