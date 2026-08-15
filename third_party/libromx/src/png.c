#include "romx_internal.h"

#include <stdlib.h>
#include <string.h>

static uint32_t read_be32(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int valid_chunk_type(const uint8_t type[4])
{
    size_t index;
    for (index = 0U; index < 4U; ++index)
        if (!((type[index] >= (uint8_t)'A' && type[index] <= (uint8_t)'Z') ||
            (type[index] >= (uint8_t)'a' && type[index] <= (uint8_t)'z'))) return 0;
    return (type[2] & UINT8_C(0x20)) == 0U;
}

static int valid_ihdr(const uint8_t data[13], uint32_t limit,
    uint32_t *width, uint32_t *height, uint8_t *color_type)
{
    const uint8_t depth = data[8];
    const uint8_t color = data[9];
    int valid_depth = 0;
    *width = read_be32(data); *height = read_be32(data + 4U); *color_type = color;
    if (*width == 0U || *height == 0U || *width > limit || *height > limit ||
        data[10] != 0U || data[11] != 0U || data[12] > 1U) return 0;
    switch (color) {
    case 0U: valid_depth = depth == 1U || depth == 2U || depth == 4U || depth == 8U || depth == 16U; break;
    case 2U: valid_depth = depth == 8U || depth == 16U; break;
    case 3U: valid_depth = depth == 1U || depth == 2U || depth == 4U || depth == 8U; break;
    case 4U: case 6U: valid_depth = depth == 8U || depth == 16U; break;
    default: break;
    }
    return valid_depth;
}

static romx_result_t validate_png(const romx_reader_t *reader,
    uint32_t *width, uint32_t *height, romx_error_t *error)
{
    static const uint8_t signature[8] = { 0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a };
    uint8_t header[8], ihdr[13], stored_crc[4];
    uint8_t *buffer = NULL;
    uint64_t position = 8U;
    int first = 1, saw_idat = 0, ended_idat = 0, saw_iend = 0, saw_plte = 0;
    uint8_t color_type = 0U;
    romx_result_t result;
    if (reader->info.cover.size == 0U) return romx_error_set(error,
        ROMX_E_COVER_ABSENT, 0, ROMX_OFFSET_UNKNOWN, "ROMX has no cover region");
    if (reader->info.cover.size > reader->max_cover_size)
        return romx_error_set(error, ROMX_E_COVER_TOO_LARGE, 0,
            reader->info.cover.offset, "cover exceeds configured size limit");
    if (reader->info.cover.size < 8U)
        return romx_error_set(error, ROMX_E_COVER_PNG, 0,
            reader->info.cover.offset, "cover has an invalid PNG signature");
    result = romx_read_exact(reader, reader->info.cover.offset,
        header, 8U, error);
    if (result != ROMX_OK) return result;
    if (memcmp(header, signature, 8U) != 0)
        return romx_error_set(error, ROMX_E_COVER_PNG, 0,
            reader->info.cover.offset, "cover has an invalid PNG signature");
    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
        ROMX_OFFSET_UNKNOWN, "failed to allocate PNG validation buffer");
    while (position < reader->info.cover.size) {
        uint32_t length, crc;
        uint64_t data_position, remaining;
        result = romx_read_exact(reader, reader->info.cover.offset + position, header, 8U, error);
        if (result != ROMX_OK) goto done;
        length = read_be32(header); data_position = position + 8U;
        remaining = reader->info.cover.size - position;
        if (remaining < 12U || length > UINT32_C(0x7fffffff) ||
            (uint64_t)length > remaining - 12U || !valid_chunk_type(header + 4U)) {
            result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                reader->info.cover.offset + position, "PNG chunk is malformed or out of range"); goto done;
        }
        if (first && (length != 13U || memcmp(header + 4U, "IHDR", 4U) != 0)) {
            result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                reader->info.cover.offset + position, "PNG IHDR must be the first chunk"); goto done;
        }
        crc = romx_crc32_begin(); crc = romx_crc32_update(crc, header + 4U, 4U);
        {
            uint64_t read_position = 0U;
            while (read_position < length) {
                uint64_t count = (uint64_t)length - read_position;
                if (count > reader->io_chunk_size) count = reader->io_chunk_size;
                result = romx_read_exact(reader, reader->info.cover.offset + data_position + read_position,
                    buffer, count, error); if (result != ROMX_OK) goto done;
                crc = romx_crc32_update(crc, buffer, (size_t)count); read_position += count;
            }
        }
        result = romx_read_exact(reader, reader->info.cover.offset + data_position + length,
            stored_crc, 4U, error); if (result != ROMX_OK) goto done;
        if (romx_crc32_finish(crc) != read_be32(stored_crc)) {
            result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                reader->info.cover.offset + data_position + length, "PNG chunk CRC mismatch"); goto done;
        }
        if (!first && (header[4] & UINT8_C(0x20)) == 0U &&
            memcmp(header + 4U, "PLTE", 4U) != 0 &&
            memcmp(header + 4U, "IDAT", 4U) != 0 &&
            memcmp(header + 4U, "IEND", 4U) != 0) {
            result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                reader->info.cover.offset + position, "PNG contains an unknown critical chunk"); goto done;
        }
        if (first) {
            result = romx_read_exact(reader, reader->info.cover.offset + data_position,
                ihdr, 13U, error); if (result != ROMX_OK) goto done;
            if (!valid_ihdr(ihdr, reader->max_cover_dimension, width, height, &color_type)) {
                result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                    reader->info.cover.offset + data_position, "PNG IHDR fields are invalid"); goto done;
            }
            first = 0;
        } else if (memcmp(header + 4U, "IHDR", 4U) == 0) {
            result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                reader->info.cover.offset + position, "PNG contains multiple IHDR chunks"); goto done;
        } else if (memcmp(header + 4U, "PLTE", 4U) == 0) {
            if (saw_plte || saw_idat || color_type == 0U || color_type == 4U ||
                length == 0U || length % 3U != 0U || length > 768U) {
                result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                    reader->info.cover.offset + position, "PNG PLTE chunk is invalid"); goto done;
            }
            saw_plte = 1;
        } else if (memcmp(header + 4U, "IDAT", 4U) == 0) {
            if (ended_idat) {
                result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                    reader->info.cover.offset + position, "PNG IDAT chunks are not consecutive"); goto done;
            }
            saw_idat = 1;
        }
        else if (memcmp(header + 4U, "IEND", 4U) == 0) {
            if (length != 0U || !saw_idat || (color_type == 3U && !saw_plte)) {
                result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
                    reader->info.cover.offset + position, "PNG IEND or required chunks are invalid"); goto done;
            }
            saw_iend = 1;
        } else if (saw_idat) ended_idat = 1;
        position += UINT64_C(12) + (uint64_t)length;
        if (saw_iend) break;
    }
    if (!saw_iend || position != reader->info.cover.size)
        result = romx_error_set(error, ROMX_E_COVER_PNG, 0,
            reader->info.cover.offset + position, "PNG is missing IEND or has trailing bytes");
    else result = ROMX_OK;
done:
    free(buffer);
    return result;
}

romx_result_t romx_validate_cover_io(const romx_io_t *io, uint64_t size,
    uint64_t max_cover_size, uint32_t max_cover_dimension,
    uint32_t io_chunk_size, uint8_t sha256[32], uint32_t *width,
    uint32_t *height, romx_error_t *error)
{
    romx_reader_t reader;
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_result_t result;

    if (io == NULL || io->struct_size < sizeof(*io) ||
        io->read_at == NULL || sha256 == NULL || width == NULL ||
        height == NULL || size == UINT64_C(0) ||
        io_chunk_size < UINT32_C(1024)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid cover writer input");
    }
    memset(&reader, 0, sizeof(reader));
    reader.io = *io;
    reader.info.cover.offset = UINT64_C(0);
    reader.info.cover.size = size;
    reader.max_cover_size = max_cover_size;
    reader.max_cover_dimension = max_cover_dimension;
    reader.io_chunk_size = io_chunk_size;
    result = romx_validate_cover_internal(&reader, &report, error);
    if (result == ROMX_OK) {
        memcpy(sha256, report.computed_cover_sha256, 32U);
        *width = report.cover_width;
        *height = report.cover_height;
    }
    return result;
}

romx_result_t romx_validate_cover_internal(const romx_reader_t *reader,
    romx_validation_report_t *report, romx_error_t *detail_error)
{
    romx_result_t result;

    result = validate_png(reader, &report->cover_width,
        &report->cover_height, detail_error);
    if (result == ROMX_E_COVER_ABSENT) {
        report->cover = ROMX_STATUS_ABSENT;
        report->cover_hashes = ROMX_STATUS_ABSENT;
        return result;
    }
    if (result != ROMX_OK) {
        report->cover = result == ROMX_E_COVER_TOO_LARGE ||
            result == ROMX_E_COVER_PNG
            ? ROMX_STATUS_INVALID : ROMX_STATUS_NOT_CHECKED;
        report->cover_hashes = ROMX_STATUS_NOT_CHECKED;
        return result;
    }
    report->cover = ROMX_STATUS_VALID;
    result = romx_hash_region(reader, reader->info.cover,
        report->computed_cover_sha256, NULL, detail_error);
    if (result != ROMX_OK) return result;
    report->cover_hashes = ROMX_STATUS_VALID;
    return ROMX_OK;
}

romx_result_t romx_reader_get_cover_info(const romx_reader_t *reader,
    romx_cover_info_t *info, romx_error_t *error)
{
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_error_t detail;
    uint32_t supplied;
    romx_result_t result;
    if (reader == NULL || info == NULL || info->struct_size < sizeof(*info))
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader or cover info is invalid");
    supplied = info->struct_size;
    result = romx_validate_cover_internal(reader, &report, &detail);
    if (result != ROMX_OK) {
        if (error != NULL) *error = detail;
        return result;
    }
    memset(info, 0, sizeof(*info));
    info->struct_size = supplied;
    info->width = report.cover_width;
    info->height = report.cover_height;
    info->size = reader->info.cover.size;
    memcpy(info->sha256, report.computed_cover_sha256, 32U);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_extract_cover_path(const romx_reader_t *reader,
    const char *destination, const romx_extract_options_t *options,
    romx_error_t *error)
{
    romx_cover_info_t info = ROMX_COVER_INFO_INIT;
    romx_result_t result = romx_validate_required_integrity(reader, error);
    if (result != ROMX_OK) return result;
    result = romx_reader_get_cover_info(reader, &info, error);
    if (result != ROMX_OK) return result;
    result = romx_extract_region_verified_path(reader, reader->info.cover,
        info.sha256, destination, options, error);
    return result;
}
