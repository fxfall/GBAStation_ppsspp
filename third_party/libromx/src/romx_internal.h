#ifndef ROMX_INTERNAL_H
#define ROMX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <romx/romx.h>
#include "json_internal.h"

struct romx_mutable_slot {
    romx_mutable_object_info_t object;
    uint16_t state;
    int usable;
};

struct romx_reader {
    romx_info_t info;
    romx_io_t io;
    void (*close_io)(void *user_data);
    uint64_t max_metadata_size;
    uint64_t max_cover_size;
    uint32_t max_cover_dimension;
    uint32_t io_chunk_size;
    romx_entry_info_t *entries;
    struct romx_mutable_slot *mutable_slots;
    uint32_t mutable_slot_count;
    romx_mutable_status_t mutable_status;
    romx_result_t (*map_payload)(
        void *user_data,
        romx_region_info_t region,
        romx_payload_mapping_t **out_mapping,
        romx_error_t *error);
};

struct romx_payload_mapping {
    const uint8_t *data;
    uint64_t size;
    void *allocation;
    size_t allocation_size;
    void (*release)(romx_payload_mapping_t *mapping);
};

struct romx_metadata {
    uint8_t *bytes;
    size_t size;
    romx_json_document_t json;
};

typedef struct romx_sha256_context {
    uint32_t state[8];
    uint8_t block[64];
    uint64_t total_size;
    size_t block_size;
} romx_sha256_context_t;

void romx_sha256_init(romx_sha256_context_t *context);
void romx_sha256_update(
    romx_sha256_context_t *context,
    const uint8_t *data,
    size_t size);
void romx_sha256_finish(
    romx_sha256_context_t *context,
    uint8_t digest[32]);

uint32_t romx_crc32_begin(void);
uint32_t romx_crc32_update(uint32_t crc, const uint8_t *data, size_t size);
uint32_t romx_crc32_finish(uint32_t crc);

romx_result_t romx_get_region_info(
    const romx_reader_t *reader,
    romx_region_t region,
    romx_region_info_t *info,
    romx_error_t *error);

romx_result_t romx_read_exact(
    const romx_reader_t *reader,
    uint64_t offset,
    void *buffer,
    uint64_t size,
    romx_error_t *error);

romx_result_t romx_hash_region(
    const romx_reader_t *reader,
    romx_region_info_t region,
    uint8_t sha256[32],
    uint32_t *crc32,
    romx_error_t *error);

romx_result_t romx_validate_metadata_internal(
    const romx_reader_t *reader,
    romx_validation_report_t *report,
    romx_error_t *detail_error);

romx_result_t romx_metadata_load_internal(
    const romx_reader_t *reader,
    romx_metadata_t **out_metadata,
    romx_error_t *error);

romx_result_t romx_validate_cover_io(
    const romx_io_t *io,
    uint64_t size,
    uint64_t max_cover_size,
    uint32_t max_cover_dimension,
    uint32_t io_chunk_size,
    uint8_t sha256[32],
    uint32_t *width,
    uint32_t *height,
    romx_error_t *error);

romx_result_t romx_validate_cover_internal(
    const romx_reader_t *reader,
    romx_validation_report_t *report,
    romx_error_t *detail_error);

romx_result_t romx_extract_region_verified_path(
    const romx_reader_t *reader,
    romx_region_info_t region,
    const uint8_t expected_sha256[32],
    const char *destination,
    const romx_extract_options_t *options,
    romx_error_t *error);

romx_result_t romx_validate_required_integrity(
    const romx_reader_t *reader,
    romx_error_t *error);

void romx_error_clear(romx_error_t *error);
romx_result_t romx_error_set(
    romx_error_t *error,
    romx_result_t code,
    int32_t system_code,
    uint64_t byte_offset,
    const char *message);

romx_result_t romx_parse_footer(
    const uint8_t footer[ROMX_FOOTER_SIZE],
    uint64_t file_size,
    romx_info_t *info,
    romx_error_t *error);

romx_result_t romx_validate_metadata_bytes(
    const uint8_t *bytes,
    size_t size,
    romx_error_t *error);

romx_result_t romx_parse_ridx(
    romx_reader_t *reader,
    romx_error_t *error);

romx_result_t romx_parse_mutable(
    romx_reader_t *reader,
    romx_error_t *error);

romx_result_t romx_reader_create(
    const romx_io_t *io,
    const romx_reader_options_t *options,
    void (*close_io)(void *user_data),
    romx_reader_t **out_reader,
    romx_error_t *error);

#endif
