#include "romx_internal.h"

#include <stdint.h>
#include <stdlib.h>

struct romx_vfs_file {
    const romx_reader_t *reader;
    uint32_t entry_index;
    uint64_t size;
    uint64_t position;
};

static romx_result_t romx_vfs_file_create(
    const romx_reader_t *reader,
    const romx_entry_info_t *entry,
    romx_vfs_file_t **out_file,
    romx_error_t *error)
{
    romx_vfs_file_t *file;

    file = (romx_vfs_file_t *)calloc(1U, sizeof(*file));
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate VFS file cursor");
    }
    file->reader = reader;
    file->entry_index = entry->index;
    file->size = entry->data_size;
    file->position = UINT64_C(0);
    *out_file = file;
    return ROMX_OK;
}

romx_result_t romx_vfs_file_open(
    const romx_reader_t *reader,
    const char *virtual_path,
    romx_vfs_file_t **out_file,
    romx_error_t *error)
{
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    romx_result_t result;

    romx_error_clear(error);
    if (out_file != NULL) *out_file = NULL;
    if (reader == NULL || virtual_path == NULL || *virtual_path == '\0' ||
        out_file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN,
            "reader, virtual path, and output must not be null");
    }
    result = romx_reader_find_entry(reader, virtual_path, &entry, error);
    if (result != ROMX_OK) return result;
    return romx_vfs_file_create(reader, &entry, out_file, error);
}

romx_result_t romx_vfs_file_open_entrypoint(
    const romx_reader_t *reader,
    romx_vfs_file_t **out_file,
    romx_error_t *error)
{
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    romx_result_t result;

    romx_error_clear(error);
    if (out_file != NULL) *out_file = NULL;
    if (reader == NULL || out_file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and output must not be null");
    }
    result = romx_reader_get_entrypoint(reader, &entry, error);
    if (result != ROMX_OK) return result;
    return romx_vfs_file_create(reader, &entry, out_file, error);
}

romx_result_t romx_vfs_file_get_size(
    const romx_vfs_file_t *file,
    uint64_t *size,
    romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || size == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "VFS file and size must not be null");
    }
    *size = file->size;
    return ROMX_OK;
}

romx_result_t romx_vfs_file_tell(
    const romx_vfs_file_t *file,
    uint64_t *position,
    romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || position == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "VFS file and position must not be null");
    }
    *position = file->position;
    return ROMX_OK;
}

static romx_result_t romx_vfs_add_offset(
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
                ROMX_OFFSET_UNKNOWN, "VFS seek overflows");
        }
        *target = base + magnitude;
        return ROMX_OK;
    }
    magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
    if (magnitude > base) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "VFS seek precedes the file start");
    }
    *target = base - magnitude;
    return ROMX_OK;
}

romx_result_t romx_vfs_file_seek(
    romx_vfs_file_t *file,
    int64_t offset,
    romx_payload_seek_position_t origin,
    uint64_t *new_position,
    romx_error_t *error)
{
    uint64_t base;
    uint64_t target;
    romx_result_t result;

    romx_error_clear(error);
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "VFS file must not be null");
    }
    switch (origin) {
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
            ROMX_OFFSET_UNKNOWN, "unknown VFS seek origin");
    }
    result = romx_vfs_add_offset(base, offset, &target, error);
    if (result != ROMX_OK) return result;
    file->position = target;
    if (new_position != NULL) *new_position = target;
    return ROMX_OK;
}

romx_result_t romx_vfs_file_read(
    romx_vfs_file_t *file,
    void *buffer,
    uint64_t size,
    uint64_t *bytes_read,
    romx_error_t *error)
{
    uint64_t count = UINT64_C(0);
    romx_result_t result;

    romx_error_clear(error);
    if (bytes_read != NULL) *bytes_read = UINT64_C(0);
    if (file == NULL || bytes_read == NULL ||
        (buffer == NULL && size != UINT64_C(0)) ||
        size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid VFS read arguments");
    }
    if (size == UINT64_C(0) || file->position >= file->size) return ROMX_OK;
    result = romx_reader_read_entry(file->reader, file->entry_index,
        file->position, buffer, size, &count, error);
    if (result != ROMX_OK) return result;
    file->position += count;
    *bytes_read = count;
    return ROMX_OK;
}

void romx_vfs_file_close(romx_vfs_file_t *file)
{
    free(file);
}
