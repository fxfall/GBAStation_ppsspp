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
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static void hash_hex(const uint8_t hash[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < 32U; ++index) {
        output[index * 2U] = digits[hash[index] >> 4];
        output[index * 2U + 1U] = digits[hash[index] & UINT8_C(0x0f)];
    }
    output[64] = '\0';
}

static int valid_extract_options(const romx_extract_options_t *options)
{
    return options == NULL || (options->struct_size >= sizeof(*options) &&
        (options->flags & ~(ROMX_EXTRACT_REPLACE_EXISTING | ROMX_EXTRACT_DURABLE)) == 0U);
}

static romx_region_info_t entrypoint_region(const romx_reader_t *reader)
{
    const romx_entry_info_t *entry =
        &reader->entries[reader->info.entrypoint_index];
    romx_region_info_t region;
    region.offset = entry->data_offset;
    region.size = entry->data_size;
    return region;
}

#if defined(_WIN32)
static wchar_t *to_wide(const char *path)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) <= 0) { free(wide); return NULL; }
    return wide;
}

static int remove_utf8(const char *path)
{
    wchar_t *wide = to_wide(path);
    int result;
    if (wide == NULL) return -1;
    result = _wremove(wide);
    free(wide);
    return result;
}
#else
static int remove_utf8(const char *path)
{
    return remove(path);
}
#endif

static romx_result_t write_region_temp(const romx_reader_t *reader,
    romx_region_info_t region, const uint8_t expected_sha256[32],
    const char *destination, romx_extract_flags_t flags,
    char **temporary_out, romx_error_t *error)
{
    static const char temporary_suffix[] = ".romx-tmp-XXXXXX";
    size_t length = strlen(destination);
    char *temporary;
    uint8_t *buffer = NULL;
    romx_sha256_context_t sha;
    uint8_t digest[32];
    uint64_t position = 0U;
    romx_result_t result = ROMX_OK;
#if defined(_WIN32)
    int descriptor = -1;
#else
    int descriptor;
#endif
    if (temporary_out != NULL) *temporary_out = NULL;
    if (length > SIZE_MAX - sizeof(temporary_suffix))
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "temporary extraction path is too long");
    temporary = (char *)malloc(length + sizeof(temporary_suffix));
    if (temporary == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate temporary path");
    memcpy(temporary, destination, length);
    memcpy(temporary + length, temporary_suffix, sizeof(temporary_suffix));
#if defined(_WIN32)
    {
        char *suffix = temporary + length + 10U;
        unsigned int attempt;
        for (attempt = 0U; attempt < 1000U; ++attempt) {
            (void)snprintf(suffix, 8U, "%06u", attempt);
            wchar_t *wide = to_wide(temporary);
            if (wide == NULL) { descriptor = -1; errno = EINVAL; break; }
            {
                int open_error = _wsopen_s(&descriptor, wide,
                    _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                    _SH_DENYNO, _S_IREAD | _S_IWRITE);
                if (open_error != 0) {
                    descriptor = -1;
                    errno = open_error;
                }
            }
            free(wide);
            if (descriptor >= 0 || errno != EEXIST) break;
        }
    }
#else
    descriptor = mkstemp(temporary);
#endif
    if (descriptor < 0) { int code = errno; free(temporary); return romx_error_set(error,
        ROMX_E_WRITE, code, ROMX_OFFSET_UNKNOWN, "failed to create unique temporary output"); }
    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate extraction buffer");
    romx_sha256_init(&sha);
    while (result == ROMX_OK && position < region.size) {
        uint64_t count = region.size - position;
        size_t written = 0U;
        if (count > reader->io_chunk_size) count = reader->io_chunk_size;
        result = romx_read_exact(reader, region.offset + position, buffer, count, error);
        if (result != ROMX_OK) break;
        romx_sha256_update(&sha, buffer, (size_t)count);
        while (written < (size_t)count) {
#if defined(_WIN32)
            int actual = _write(descriptor, buffer + written,
                (unsigned int)((size_t)count - written));
#else
            ssize_t actual = write(descriptor, buffer + written, (size_t)count - written);
#endif
            if (actual < 0) { if (errno == EINTR) continue; result = romx_error_set(error,
                ROMX_E_WRITE, errno, position + written, "failed to write extracted payload"); break; }
            if (actual == 0) { result = romx_error_set(error, ROMX_E_WRITE, 0,
                position + written, "zero-length payload write"); break; }
            written += (size_t)actual;
        }
        position += count;
    }
    if (result == ROMX_OK) {
        romx_sha256_finish(&sha, digest);
        if (memcmp(digest, expected_sha256, 32U) != 0)
            result = romx_error_set(error, ROMX_E_EXTRACT_HASH, 0,
                region.offset, "extracted region SHA-256 does not match expected value");
    }
    if (result == ROMX_OK && (flags & ROMX_EXTRACT_DURABLE) != 0U) {
#if defined(_WIN32)
        if (_commit(descriptor) != 0)
#else
        if (fsync(descriptor) != 0)
#endif
            result = romx_error_set(error, ROMX_E_WRITE, errno,
                ROMX_OFFSET_UNKNOWN, "failed to sync extracted payload");
    }
#if defined(_WIN32)
    if (_close(descriptor) != 0 && result == ROMX_OK)
#else
    if (close(descriptor) != 0 && result == ROMX_OK)
#endif
        result = romx_error_set(error, ROMX_E_WRITE, errno,
            ROMX_OFFSET_UNKNOWN, "failed to close extracted payload");
    free(buffer);
    if (result != ROMX_OK) { (void)remove_utf8(temporary); free(temporary); return result; }
    *temporary_out = temporary;
    return ROMX_OK;
}

static romx_result_t publish_temp(const char *temporary, const char *destination,
    int replace, romx_error_t *error)
{
#if defined(_WIN32)
    wchar_t *source = to_wide(temporary), *target = to_wide(destination);
    DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (source == NULL || target == NULL || !MoveFileExW(source, target, flags)) {
        DWORD code = GetLastError(); free(source); free(target); (void)remove_utf8(temporary);
        return romx_error_set(error, code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? ROMX_E_EXISTS : ROMX_E_ATOMIC_RENAME,
            (int32_t)code, ROMX_OFFSET_UNKNOWN, "failed to publish extracted payload");
    }
    free(source); free(target);
#else
    if (replace) {
        if (rename(temporary, destination) != 0) { int code=errno; (void)unlink(temporary);
            return romx_error_set(error, ROMX_E_ATOMIC_RENAME, code,
                ROMX_OFFSET_UNKNOWN, "failed to atomically replace extracted payload"); }
    } else {
        if (link(temporary, destination) != 0) { int code=errno; (void)unlink(temporary);
            return romx_error_set(error, code == EEXIST ? ROMX_E_EXISTS : ROMX_E_ATOMIC_RENAME,
                code, ROMX_OFFSET_UNKNOWN, "failed to atomically publish extracted payload"); }
        (void)unlink(temporary);
    }
#endif
    return ROMX_OK;
}

romx_result_t romx_extract_payload_path(const romx_reader_t *reader,
    const char *destination, const romx_extract_options_t *options,
    romx_error_t *error)
{
    romx_region_info_t region;
    char *temporary = NULL;
    uint8_t payload_sha256[32];
    romx_extract_flags_t flags = options != NULL ? options->flags : UINT32_C(0);
    romx_result_t result;
    if (reader == NULL || destination == NULL || destination[0] == '\0' ||
        !valid_extract_options(options)) return romx_error_set(error,
            ROMX_E_INVALID_ARGUMENT, 0, ROMX_OFFSET_UNKNOWN,
            "invalid payload extraction arguments");
    result = romx_validate_required_integrity(reader, error);
    if (result != ROMX_OK) return result;
    region = entrypoint_region(reader);
    result = romx_hash_region(reader, region,
        payload_sha256, NULL, error);
    if (result != ROMX_OK) return result;
    result = write_region_temp(reader, region, payload_sha256,
        destination, flags, &temporary, error);
    if (result != ROMX_OK) return result;
    result = publish_temp(temporary, destination,
        (flags & ROMX_EXTRACT_REPLACE_EXISTING) != 0U, error);
    free(temporary);
    if (result == ROMX_OK) romx_error_clear(error);
    return result;
}

romx_result_t romx_extract_region_verified_path(const romx_reader_t *reader,
    romx_region_info_t region, const uint8_t expected_sha256[32],
    const char *destination, const romx_extract_options_t *options,
    romx_error_t *error)
{
    char *temporary = NULL;
    romx_extract_flags_t flags = options != NULL ? options->flags : UINT32_C(0);
    romx_result_t result;
    if (reader == NULL || expected_sha256 == NULL || destination == NULL ||
        destination[0] == '\0' || !valid_extract_options(options))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid region extraction arguments");
    result = write_region_temp(reader, region, expected_sha256, destination,
        flags, &temporary, error);
    if (result != ROMX_OK) return result;
    result = publish_temp(temporary, destination,
        (flags & ROMX_EXTRACT_REPLACE_EXISTING) != 0U, error);
    free(temporary);
    if (result == ROMX_OK) romx_error_clear(error);
    return result;
}

static int existing_file_matches(const char *path, uint64_t expected_size,
    const uint8_t expected_hash[32])
{
    FILE *file;
    uint8_t buffer[65536];
    romx_sha256_context_t sha;
    uint8_t digest[32];
    uint64_t size = 0U;
#if defined(_WIN32)
    {
        wchar_t *wide = to_wide(path);
        if (wide == NULL) return 0;
        if (_wfopen_s(&file, wide, L"rb") != 0) file = NULL;
        free(wide);
    }
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) return 0;
    romx_sha256_init(&sha);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), file);
        if (count != 0U) { romx_sha256_update(&sha, buffer, count); size += count; }
        if (count < sizeof(buffer)) { if (ferror(file)) { fclose(file); return 0; } break; }
    }
    fclose(file); romx_sha256_finish(&sha, digest);
    return size == expected_size && memcmp(digest, expected_hash, 32U) == 0;
}

romx_result_t romx_extract_payload_cache(const romx_reader_t *reader,
    const char *directory, const romx_extract_options_t *options,
    char *result_path, uint64_t result_capacity, uint64_t *required_size,
    romx_error_t *error)
{
    char hash[65];
    const char *extension = "rom";
    uint8_t payload_sha256[32];
    char separator;
    size_t directory_length, path_size;
    char *path;
    romx_region_info_t region;
    const char *dot;
    romx_extract_options_t replace = ROMX_EXTRACT_OPTIONS_INIT;
    romx_result_t result;
    if (reader == NULL || directory == NULL || directory[0] == '\0' ||
        required_size == NULL ||
        !valid_extract_options(options) || (result_path == NULL && result_capacity != 0U))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid cache extraction arguments");
    result = romx_validate_required_integrity(reader, error);
    if (result != ROMX_OK) return result;
    region = entrypoint_region(reader);
    result = romx_hash_region(reader, region,
        payload_sha256, NULL, error);
    if (result != ROMX_OK) return result;
    hash_hex(payload_sha256, hash);
    dot = strrchr(reader->entries[reader->info.entrypoint_index].path, '.');
    if (dot != NULL && dot[1] != '\0' && strlen(dot + 1U) <= 16U) {
        extension = dot + 1U;
    }
    directory_length = strlen(directory);
    if (directory_length > SIZE_MAX - (1U + 64U + 1U +
            strlen(extension) + 1U)) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "cache result path is too long");
    }
    path_size = directory_length + 1U + 64U + 1U + strlen(extension) + 1U;
    *required_size = (uint64_t)path_size;
    if (result_path == NULL || result_capacity < path_size)
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN, "cache result path buffer is too small");
    path = (char *)malloc(path_size);
    if (path == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate cache path");
#if defined(_WIN32)
    separator = '\\';
#else
    separator = '/';
#endif
    (void)snprintf(path, path_size, "%s%c%s.%s", directory, separator,
        hash, extension);
#if defined(_WIN32)
    { wchar_t *wide=to_wide(directory); if (wide==NULL || (_wmkdir(wide)!=0 && errno!=EEXIST)) { free(wide);free(path);return romx_error_set(error,ROMX_E_WRITE,errno,ROMX_OFFSET_UNKNOWN,"failed to create cache directory"); } free(wide); }
#else
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) { int code=errno;free(path);return romx_error_set(error,ROMX_E_WRITE,code,ROMX_OFFSET_UNKNOWN,"failed to create cache directory"); }
#endif
    if (!existing_file_matches(path, region.size, payload_sha256)) {
        replace.flags = ROMX_EXTRACT_REPLACE_EXISTING |
            (options != NULL ? (options->flags & ROMX_EXTRACT_DURABLE) : 0U);
        result = romx_extract_region_verified_path(reader, region,
            payload_sha256, path, &replace, error);
        if (result != ROMX_OK && result != ROMX_E_EXISTS) { free(path); return result; }
        if (!existing_file_matches(path, region.size, payload_sha256)) {
            free(path); return romx_error_set(error, ROMX_E_EXTRACT_HASH, 0,
                ROMX_OFFSET_UNKNOWN, "published cache payload failed verification");
        }
    }
    memcpy(result_path, path, path_size); free(path); romx_error_clear(error); return ROMX_OK;
}
