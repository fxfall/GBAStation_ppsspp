#include "romx_internal.h"

#include <string.h>
#include <stdlib.h>

static romx_result_t romx_validate_entry_crc32_values(
    const romx_reader_t *reader,
    romx_validation_report_t *report,
    romx_error_t *error)
{
    uint8_t *buffer;
    uint32_t index;
    int checked = 0;

    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate entry CRC32 buffer");
    }
    for (index = 0U; index < reader->info.entry_count; ++index) {
        const romx_entry_info_t *entry = &reader->entries[index];
        uint64_t position = UINT64_C(0);
        uint32_t crc;

        if ((entry->flags & ROMX_RIDX_HAS_CRC32) == UINT32_C(0)) continue;
        checked = 1;
        crc = romx_crc32_begin();
        while (position < entry->data_size) {
            uint64_t count = entry->data_size - position;
            uint64_t bytes_read = UINT64_C(0);
            romx_result_t result;
            if (count > reader->io_chunk_size) count = reader->io_chunk_size;
            result = romx_reader_read_entry(reader, index, position, buffer,
                count, &bytes_read, error);
            if (result != ROMX_OK) {
                free(buffer);
                return result;
            }
            crc = romx_crc32_update(crc, buffer, (size_t)bytes_read);
            position += bytes_read;
        }
        crc = romx_crc32_finish(crc);
        if (crc != entry->crc32) {
            report->entry_crc32 = ROMX_STATUS_INVALID;
            free(buffer);
            return romx_error_set(error, ROMX_E_ENTRY_CRC, 0,
                entry->data_offset, "RIDX entry CRC32 does not match its payload bytes");
        }
    }
    free(buffer);
    report->entry_crc32 = checked ? ROMX_STATUS_VALID : ROMX_STATUS_ABSENT;
    return ROMX_OK;
}

romx_result_t romx_reader_validate(
    const romx_reader_t *reader,
    romx_validate_flags_t flags,
    romx_validation_report_t *report,
    romx_error_t *error)
{
    uint32_t supplied_size;
    romx_result_t result = ROMX_OK;
    romx_error_t optional_error;

    romx_error_clear(error);
    if (reader == NULL || report == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN,
            "reader and validation report must not be null");
    }
    if ((flags & ~ROMX_VALIDATE_ALL) != UINT32_C(0)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "unknown validation flags");
    }
    supplied_size = report->struct_size;
    if (supplied_size < sizeof(*report)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "validation report structure is too small");
    }

    memset(report, 0, sizeof(*report));
    report->struct_size = supplied_size;
    report->structure = ROMX_STATUS_VALID;

    if ((flags & ROMX_VALIDATE_PAYLOAD_HASHES) != 0U) {
        result = romx_hash_region(reader, reader->info.payload,
            report->computed_payload_sha256,
            &report->computed_payload_crc32, error);
        if (result != ROMX_OK) {
            return result;
        }
        report->payload_hashes = ROMX_STATUS_VALID;
    }

    if ((flags & ROMX_VALIDATE_IMMUTABLE_SHA256) != 0U) {
        if (reader->info.immutable_hash_algorithm == ROMX_IMMUTABLE_HASH_NONE) {
            report->immutable_sha256 = ROMX_STATUS_ABSENT;
        } else {
            romx_region_info_t immutable;
            immutable.offset = UINT64_C(0);
            immutable.size = reader->info.immutable_size;
            result = romx_hash_region(reader, immutable,
                report->computed_immutable_sha256, NULL, error);
            if (result != ROMX_OK) {
                return result;
            }
            report->immutable_sha256 = memcmp(report->computed_immutable_sha256,
                reader->info.immutable_sha256, 32U) == 0
                ? ROMX_STATUS_VALID
                : ROMX_STATUS_INVALID;
        }
    }

    if ((flags & ROMX_VALIDATE_ENTRY_CRC32) != 0U) {
        result = romx_validate_entry_crc32_values(reader, report, error);
        if (result != ROMX_OK) return result;
    }

    if ((flags & ROMX_VALIDATE_METADATA) != 0U) {
        romx_error_clear(&optional_error);
        result = romx_validate_metadata_internal(
            reader, report, &optional_error);
        report->metadata_result = result;
        if (result != ROMX_OK && result != ROMX_E_METADATA_ABSENT &&
            result != ROMX_E_METADATA_TOO_LARGE &&
            result != ROMX_E_METADATA_UTF8 &&
            result != ROMX_E_METADATA_JSON &&
            result != ROMX_E_METADATA_SCHEMA) {
            if (error != NULL) {
                *error = optional_error;
            }
            return result;
        }
    }

    if ((flags & ROMX_VALIDATE_COVER) != 0U) {
        romx_error_clear(&optional_error);
        result = romx_validate_cover_internal(reader, report, &optional_error);
        report->cover_result = result;
        if (result != ROMX_OK && result != ROMX_E_COVER_ABSENT &&
            result != ROMX_E_COVER_TOO_LARGE &&
            result != ROMX_E_COVER_PNG) {
            if (error != NULL) {
                *error = optional_error;
            }
            return result;
        }
    }

    if (report->immutable_sha256 == ROMX_STATUS_INVALID) {
        return romx_error_set(error, ROMX_E_IMMUTABLE_HASH, 0,
            UINT64_C(0), "immutable SHA-256 does not match footer");
    }

    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_validate_required_integrity(
    const romx_reader_t *reader,
    romx_error_t *error)
{
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;

    return romx_reader_validate(reader, ROMX_VALIDATE_IMMUTABLE_SHA256,
        &report, error);
}
