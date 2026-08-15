#include "romx_internal.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

struct romx_payload_file {
    romx_reader_t *reader;
    uint32_t entry_index;
    uint64_t size;
    uint64_t position;
};

static romx_result_t romx_payload_file_validate_options(
    const romx_payload_file_options_t *options,
    romx_error_t *error)
{
    if (options == NULL) {
        return ROMX_OK;
    }
    if (options->struct_size < sizeof(*options)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "payload file options structure is too small");
    }
    if ((options->flags & ~ROMX_PAYLOAD_FILE_VALIDATE_IMMUTABLE_SHA256) != 0U ||
        options->reserved != UINT32_C(0)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN,
            "payload file options contain unsupported fields");
    }
    return ROMX_OK;
}

romx_result_t romx_payload_file_open_path(
    const char *utf8_path,
    const romx_reader_options_t *reader_options,
    const romx_payload_file_options_t *options,
    romx_payload_file_t **out_file,
    romx_error_t *error)
{
    romx_payload_file_t *file;
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_result_t result;

    romx_error_clear(error);
    if (out_file != NULL) {
        *out_file = NULL;
    }
    if (utf8_path == NULL || *utf8_path == '\0' || out_file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "payload file path and output must not be null");
    }
    result = romx_payload_file_validate_options(options, error);
    if (result != ROMX_OK) {
        return result;
    }

    file = (romx_payload_file_t *)calloc(1U, sizeof(*file));
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate payload file handle");
    }
    result = romx_reader_open_path(utf8_path, reader_options,
        &file->reader, error);
    if (result != ROMX_OK) {
        free(file);
        return result;
    }
    if (options != NULL &&
        (options->flags & ROMX_PAYLOAD_FILE_VALIDATE_IMMUTABLE_SHA256) != 0U) {
        result = romx_reader_validate(file->reader,
            ROMX_VALIDATE_IMMUTABLE_SHA256, &report, error);
        if (result != ROMX_OK) {
            romx_reader_close(file->reader);
            free(file);
            return result;
        }
    }
    {
        romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
        result = romx_reader_get_entrypoint(file->reader, &entry, error);
        if (result != ROMX_OK) {
            romx_reader_close(file->reader);
            free(file);
            return result;
        }
        file->entry_index = entry.index;
        file->size = entry.data_size;
    }
    file->position = UINT64_C(0);
    *out_file = file;
    return ROMX_OK;
}

romx_result_t romx_payload_file_get_size(
    const romx_payload_file_t *file,
    uint64_t *size,
    romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || size == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "payload file and size must not be null");
    }
    *size = file->size;
    return ROMX_OK;
}

romx_result_t romx_payload_file_tell(
    const romx_payload_file_t *file,
    uint64_t *position,
    romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || position == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "payload file and position must not be null");
    }
    *position = file->position;
    return ROMX_OK;
}

static romx_result_t romx_payload_file_add_offset(
    uint64_t base,
    int64_t offset,
    uint64_t *target,
    romx_error_t *error)
{
    uint64_t magnitude;

    if (offset >= 0) {
        magnitude = (uint64_t)offset;
        if (base > UINT64_MAX - magnitude) {
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "payload file seek overflows");
        }
        *target = base + magnitude;
        return ROMX_OK;
    }

    /* Avoid negating INT64_MIN directly. */
    magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
    if (magnitude > base) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "payload file seek precedes the start");
    }
    *target = base - magnitude;
    return ROMX_OK;
}

romx_result_t romx_payload_file_seek(
    romx_payload_file_t *file,
    int64_t offset,
    romx_payload_seek_position_t position,
    uint64_t *new_position,
    romx_error_t *error)
{
    uint64_t base;
    uint64_t target;
    romx_result_t result;

    romx_error_clear(error);
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "payload file must not be null");
    }
    switch (position) {
    case ROMX_PAYLOAD_SEEK_START:
        base = UINT64_C(0);
        break;
    case ROMX_PAYLOAD_SEEK_CURRENT:
        base = file->position;
        break;
    case ROMX_PAYLOAD_SEEK_END:
        base = file->size;
        break;
    default:
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "unknown payload file seek position");
    }
    result = romx_payload_file_add_offset(base, offset, &target, error);
    if (result != ROMX_OK) {
        return result;
    }
    file->position = target;
    if (new_position != NULL) {
        *new_position = target;
    }
    return ROMX_OK;
}

romx_result_t romx_payload_file_read(
    romx_payload_file_t *file,
    void *buffer,
    uint64_t size,
    uint64_t *bytes_read,
    romx_error_t *error)
{
    romx_result_t result;
    uint64_t count = UINT64_C(0);

    romx_error_clear(error);
    if (bytes_read != NULL) {
        *bytes_read = UINT64_C(0);
    }
    if (file == NULL || bytes_read == NULL ||
        (buffer == NULL && size != UINT64_C(0)) ||
        size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid payload file read arguments");
    }
    if (size == UINT64_C(0) || file->position >= file->size) {
        return ROMX_OK;
    }
    result = romx_reader_read_entry(file->reader, file->entry_index,
        file->position, buffer, size, &count, error);
    if (result != ROMX_OK) {
        return result;
    }
    file->position += count;
    *bytes_read = count;
    return ROMX_OK;
}

void romx_payload_file_close(romx_payload_file_t *file)
{
    if (file == NULL) {
        return;
    }
    romx_reader_close(file->reader);
    free(file);
}
