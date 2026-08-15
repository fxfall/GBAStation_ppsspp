#include "romx_internal.h"

#include <stdlib.h>

romx_result_t romx_get_region_info(
    const romx_reader_t *reader,
    romx_region_t region,
    romx_region_info_t *info,
    romx_error_t *error)
{
    if (reader == NULL || info == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and region info must not be null");
    }
    switch (region) {
    case ROMX_REGION_PAYLOAD: *info = reader->info.payload; break;
    case ROMX_REGION_METADATA: *info = reader->info.metadata; break;
    case ROMX_REGION_COVER: *info = reader->info.cover; break;
    case ROMX_REGION_PAYLOAD_INDEX: *info = reader->info.payload_index; break;
    case ROMX_REGION_MUTABLE: *info = reader->info.mutable_region; break;
    case ROMX_REGION_IMMUTABLE:
        info->offset = UINT64_C(0);
        info->size = reader->info.immutable_size;
        break;
    default:
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "unknown ROMX region");
    }
    return ROMX_OK;
}

romx_result_t romx_read_exact(
    const romx_reader_t *reader,
    uint64_t offset,
    void *buffer,
    uint64_t size,
    romx_error_t *error)
{
    uint64_t bytes_read = UINT64_C(0);
    romx_result_t result;

    result = reader->io.read_at(reader->io.user_data, offset, buffer, size,
        &bytes_read, error);
    if (result != ROMX_OK) return result;
    if (bytes_read != size) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0, offset + bytes_read,
            "ROMX region read was truncated");
    }
    return ROMX_OK;
}

romx_result_t romx_reader_read_region(
    const romx_reader_t *reader,
    romx_region_t region,
    uint64_t region_offset,
    void *buffer,
    uint64_t buffer_size,
    uint64_t *bytes_read,
    romx_error_t *error)
{
    /* Initialize defensively: GCC cannot infer that the helper populates all
     * fields on every successful return through its out-parameter. */
    romx_region_info_t info = { UINT64_C(0), UINT64_C(0) };
    uint64_t count;
    romx_result_t result;

    romx_error_clear(error);
    if (reader == NULL || bytes_read == NULL ||
        (buffer == NULL && buffer_size != UINT64_C(0)) ||
        buffer_size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid region read arguments");
    }
    *bytes_read = UINT64_C(0);
    result = romx_get_region_info(reader, region, &info, error);
    if (result != ROMX_OK) return result;
    if (region_offset > info.size) {
        return romx_error_set(error, ROMX_E_RANGE, 0, region_offset,
            "region-relative offset exceeds region size");
    }
    count = info.size - region_offset;
    if (count > buffer_size) count = buffer_size;
    if (count == UINT64_C(0)) return ROMX_OK;
    result = reader->io.read_at(reader->io.user_data,
        info.offset + region_offset, buffer, count, bytes_read, error);
    if (result != ROMX_OK) return result;
    if (*bytes_read != count) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            info.offset + region_offset + *bytes_read,
            "ROMX region read was truncated");
    }
    return ROMX_OK;
}

romx_result_t romx_reader_copy_region(
    const romx_reader_t *reader,
    romx_region_t region,
    const romx_sink_t *sink,
    romx_error_t *error)
{
    /* Initialize defensively: GCC cannot infer that the helper populates all
     * fields on every successful return through its out-parameter. */
    romx_region_info_t info = { UINT64_C(0), UINT64_C(0) };
    uint8_t *buffer;
    uint64_t position = UINT64_C(0);
    romx_result_t result;

    romx_error_clear(error);
    if (reader == NULL || sink == NULL || sink->struct_size < sizeof(*sink) ||
        sink->write == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "sink structure is incomplete");
    }
    result = romx_get_region_info(reader, region, &info, error);
    if (result != ROMX_OK) return result;
    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate streaming buffer");
    }
    while (position < info.size) {
        uint64_t count = info.size - position;
        if (count > reader->io_chunk_size) count = reader->io_chunk_size;
        result = romx_read_exact(reader, info.offset + position, buffer, count, error);
        if (result != ROMX_OK) break;
        result = sink->write(sink->user_data, buffer, count, error);
        if (result != ROMX_OK) break;
        position += count;
    }
    if (result == ROMX_OK && sink->flush != NULL) {
        result = sink->flush(sink->user_data, error);
    }
    free(buffer);
    return result;
}

romx_result_t romx_hash_region(
    const romx_reader_t *reader,
    romx_region_info_t region,
    uint8_t sha256[32],
    uint32_t *crc32,
    romx_error_t *error)
{
    romx_sha256_context_t sha;
    uint32_t crc = romx_crc32_begin();
    uint8_t *buffer;
    uint64_t position = UINT64_C(0);
    romx_result_t result = ROMX_OK;

    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate hash buffer");
    }
    romx_sha256_init(&sha);
    while (position < region.size) {
        uint64_t count = region.size - position;
        if (count > reader->io_chunk_size) count = reader->io_chunk_size;
        result = romx_read_exact(reader, region.offset + position, buffer, count, error);
        if (result != ROMX_OK) break;
        romx_sha256_update(&sha, buffer, (size_t)count);
        if (crc32 != NULL) crc = romx_crc32_update(crc, buffer, (size_t)count);
        position += count;
    }
    if (result == ROMX_OK) {
        romx_sha256_finish(&sha, sha256);
        if (crc32 != NULL) *crc32 = romx_crc32_finish(crc);
    }
    free(buffer);
    return result;
}
