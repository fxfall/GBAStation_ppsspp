#include "romx_internal.h"

#include <string.h>

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
    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static int bytes_are_zero(const uint8_t *bytes, size_t size)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) return 0;
    }
    return 1;
}

romx_result_t romx_parse_footer(
    const uint8_t footer[ROMX_FOOTER_SIZE],
    uint64_t file_size,
    romx_info_t *info,
    romx_error_t *error)
{
    uint8_t checked[ROMX_FOOTER_SIZE];
    uint64_t footer_offset;
    uint64_t mutable_capacity;
    uint32_t expected_crc;
    uint32_t actual_crc;

    if (footer == NULL || info == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "footer and info must not be null");
    }
    if (file_size < ROMX_FOOTER_SIZE) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0, file_size,
            "file is shorter than the ROMX footer");
    }
    footer_offset = file_size - ROMX_FOOTER_SIZE;
    if (memcmp(footer, "ROMX", 4U) != 0 ||
        read_le32(footer + 0x04U) != ROMX_FORMAT_VERSION) {
        return romx_error_set(error, ROMX_E_INVALID_FOOTER, 0,
            footer_offset, "footer is not ROMX 0.2.0");
    }
    memcpy(checked, footer, sizeof(checked));
    expected_crc = read_le32(footer + 0x50U);
    memset(checked + 0x50U, 0, 4U);
    actual_crc = romx_crc32_begin();
    actual_crc = romx_crc32_update(actual_crc, checked, sizeof(checked));
    actual_crc = romx_crc32_finish(actual_crc);
    if (actual_crc != expected_crc || !bytes_are_zero(footer + 0x54U, 44U)) {
        return romx_error_set(error, ROMX_E_INVALID_FOOTER, 0,
            footer_offset + 0x50U, "footer CRC32 or reserved bytes are invalid");
    }

    memset(info, 0, sizeof(*info));
    info->struct_size = (uint32_t)sizeof(*info);
    info->version = ROMX_FORMAT_VERSION;
    info->file_size = file_size;
    info->footer.offset = footer_offset;
    info->footer.size = ROMX_FOOTER_SIZE;
    info->footer_crc32 = expected_crc;
    info->payload.offset = UINT64_C(0);
    info->payload.size = read_le64(footer + 0x08U);
    info->metadata.size = read_le64(footer + 0x10U);
    info->cover.size = read_le64(footer + 0x18U);
    mutable_capacity = read_le64(footer + 0x20U);
    info->platform_id = read_le16(footer + 0x28U);
    info->launch_format_id = read_le16(footer + 0x2AU);
    info->immutable_hash_algorithm = read_le32(footer + 0x2CU);
    memcpy(info->immutable_sha256, footer + 0x30U, 32U);

    if (info->payload.size == UINT64_C(0) ||
        info->payload.size > footer_offset ||
        footer_offset - info->payload.size < ROMX_RIDX_HEADER_SIZE) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            footer_offset + 0x08U, "payload is empty or RIDX cannot fit");
    }
    if (mutable_capacity != UINT64_C(0)) {
        if (mutable_capacity < UINT64_C(12288) ||
            mutable_capacity % UINT64_C(4096) != UINT64_C(0) ||
            mutable_capacity > footer_offset) {
            return romx_error_set(error, ROMX_E_RANGE, 0,
                footer_offset + 0x20U, "mutable capacity is invalid");
        }
        info->mutable_region.offset = footer_offset - mutable_capacity;
        info->mutable_region.size = mutable_capacity;
        info->immutable_size = info->mutable_region.offset;
    } else {
        info->immutable_size = footer_offset;
    }
    if (info->payload.size > info->immutable_size ||
        info->immutable_size - info->payload.size < ROMX_RIDX_HEADER_SIZE) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            footer_offset + 0x08U, "RIDX exceeds immutable content");
    }
    if (info->platform_id == UINT16_C(0xffff) ||
        info->launch_format_id == UINT16_C(0xffff)) {
        return romx_error_set(error, ROMX_E_INVALID_FOOTER, 0,
            footer_offset + 0x28U, "footer contains a prohibited registry value");
    }
    if (info->immutable_hash_algorithm == ROMX_IMMUTABLE_HASH_NONE) {
        if (!bytes_are_zero(info->immutable_sha256, 32U)) {
            return romx_error_set(error, ROMX_E_INVALID_FOOTER, 0,
                footer_offset + 0x30U, "disabled immutable hash must be zero");
        }
    } else if (info->immutable_hash_algorithm != ROMX_IMMUTABLE_HASH_SHA256) {
        return romx_error_set(error, ROMX_E_INVALID_FOOTER, 0,
            footer_offset + 0x2CU, "immutable hash algorithm is unknown");
    }
    info->payload_index.offset = info->payload.size;
    return ROMX_OK;
}
