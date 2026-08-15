#include "romx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_length_between(const romx_json_document_t *json,
    int token, size_t minimum, size_t maximum)
{
    size_t length;

    if (token < 0 || json->tokens[token].type != ROMX_JSON_STRING) {
        return 0;
    }
    length = romx_json_string_characters(json, token);
    return length >= minimum && length <= maximum;
}

static int string_is_one_of(const romx_json_document_t *json, int token,
    const char *const *values, size_t count)
{
    size_t index;

    if (token < 0 || json->tokens[token].type != ROMX_JSON_STRING) {
        return 0;
    }
    for (index = 0U; index < count; ++index) {
        if (romx_json_string_equals(json, token, values[index])) {
            return 1;
        }
    }
    return 0;
}

static int object_has_duplicate_keys(
    const romx_json_document_t *json, int object)
{
    int first = -1;

    for (;;) {
        int first_value;
        int second;
        first = romx_json_next_direct_child(json, object, first);
        if (first < 0) {
            return 0;
        }
        first_value = romx_json_next_direct_child(json, object, first);
        if (first_value < 0) {
            return 1;
        }
        second = first_value;
        for (;;) {
            int second_value;

            second = romx_json_next_direct_child(json, object, second);
            if (second < 0) {
                break;
            }
            second_value = romx_json_next_direct_child(json, object, second);
            if (second_value < 0) {
                return 1;
            }
            if (romx_json_strings_equal(json, first, second)) {
                return 1;
            }
            second = second_value;
        }
        first = first_value;
    }
}

static int document_has_duplicate_keys(const romx_json_document_t *json)
{
    size_t index;

    for (index = 0U; index < json->token_count; ++index) {
        if (json->tokens[index].type == ROMX_JSON_OBJECT &&
            object_has_duplicate_keys(json, (int)index)) {
            return 1;
        }
    }
    return 0;
}

static int valid_hex_string(
    const romx_json_document_t *json, int token, size_t length)
{
    char value[9];
    size_t required = 0U;
    size_t index;

    if (length + 1U > sizeof(value) ||
        !string_length_between(json, token, length, length) ||
        !romx_json_copy_string(json, token, value, sizeof(value),
            &required) || required != length + 1U) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
            (value[index] >= 'a' && value[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int string_tokens_equal(
    const romx_json_document_t *json, int first, int second)
{
    return romx_json_strings_equal(json, first, second);
}

static int valid_string_array(const romx_json_document_t *json, int array,
    size_t maximum_items, size_t maximum_length)
{
    int item = -1;
    size_t count = 0U;

    if (array < 0 || json->tokens[array].type != ROMX_JSON_ARRAY) {
        return 0;
    }
    for (;;) {
        int previous = -1;

        item = romx_json_next_direct_child(json, array, item);
        if (item < 0) {
            break;
        }
        ++count;
        if (count > maximum_items ||
            !string_length_between(json, item, 0U, maximum_length)) {
            return 0;
        }
        for (;;) {
            previous = romx_json_next_direct_child(json, array, previous);
            if (previous < 0 || previous == item) {
                break;
            }
            if (string_tokens_equal(json, previous, item)) {
                return 0;
            }
        }
    }
    return 1;
}

static int valid_release_date(const romx_json_document_t *json, int token)
{
    char value[11];
    size_t required = 0U;
    size_t index;

    if (!romx_json_copy_string(json, token, value, sizeof(value), &required)) {
        return 0;
    }
    if (required != 5U && required != 8U && required != 11U) {
        return 0;
    }
    for (index = 0U; index < required - 1U; ++index) {
        if (index == 4U || index == 7U) {
            if (value[index] != '-') {
                return 0;
            }
        } else if (value[index] < '0' || value[index] > '9') {
            return 0;
        }
    }
    return 1;
}

static int valid_boolean(const romx_json_document_t *json, int token)
{
    return token >= 0 && (json->tokens[token].type == ROMX_JSON_TRUE ||
        json->tokens[token].type == ROMX_JSON_FALSE);
}

static int valid_cover_object(const romx_json_document_t *json, int object)
{
    int key = -1;

    if (object < 0 || json->tokens[object].type != ROMX_JSON_OBJECT) {
        return 0;
    }
    for (;;) {
        int value;
        int valid = 1;

        key = romx_json_next_direct_child(json, object, key);
        if (key < 0) break;
        value = romx_json_next_direct_child(json, object, key);
        if (value < 0) return 0;
        if (romx_json_string_equals(json, key, "mime_type")) {
            valid = string_length_between(json, value, 1U, 64U) &&
                romx_json_string_equals(json, value, "image/png");
        } else if (romx_json_string_equals(json, key, "width") ||
            romx_json_string_equals(json, key, "height")) {
            int64_t dimension = 0;
            valid = romx_json_integer(json, value, &dimension) &&
                dimension >= 1 && dimension <= 8192;
        } else {
            valid = 0;
        }
        if (!valid) return 0;
        key = value;
    }
    return 1;
}

static int valid_schema(const romx_json_document_t *json)
{
    static const char *const dumps[] = {
        "unknown", "good", "bad", "overdump", "hack", "translation",
        "homebrew"
    };
    int key = -1;
    int required_schema = 0;
    int required_name = 0;

    if (json->token_count == 0U ||
        json->tokens[0].type != ROMX_JSON_OBJECT ||
        document_has_duplicate_keys(json)) {
        return 0;
    }

    for (;;) {
        int value;
        int valid = 1;

        key = romx_json_next_direct_child(json, 0, key);
        if (key < 0) {
            break;
        }
        value = romx_json_next_direct_child(json, 0, key);
        if (value < 0) {
            return 0;
        }

        if (romx_json_string_equals(json, key, "schema_version")) {
            valid = romx_json_string_equals(json, value, "0.2.0");
            required_schema = valid;
        } else if (romx_json_string_equals(json, key, "name")) {
            valid = string_length_between(json, value, 1U, 512U);
            required_name = valid;
        } else if (romx_json_string_equals(json, key, "crc32")) {
            valid = valid_hex_string(json, value, 8U);
        } else if (romx_json_string_equals(json, key, "origin_crc32")) {
            valid = valid_hex_string(json, value, 8U);
        } else if (romx_json_string_equals(json, key, "serial") ||
            romx_json_string_equals(json, key, "origin") ||
            romx_json_string_equals(json, key, "category")) {
            valid = string_length_between(json, value, 0U, 128U);
        } else if (romx_json_string_equals(json, key, "developer") ||
            romx_json_string_equals(json, key, "publisher") ||
            romx_json_string_equals(json, key, "franchise") ||
            romx_json_string_equals(json, key, "language") ||
            romx_json_string_equals(json, key, "enhancement_hw")) {
            valid = string_length_between(json, value, 0U, 256U);
        } else if (romx_json_string_equals(json, key, "media")) {
            valid = string_length_between(json, value, 0U, 64U);
        } else if (romx_json_string_equals(json, key, "description")) {
            valid = string_length_between(json, value, 0U, 32768U);
        } else if (romx_json_string_equals(json, key, "release_date")) {
            valid = valid_release_date(json, value);
        } else if (romx_json_string_equals(json, key, "genre") ||
            romx_json_string_equals(json, key, "region")) {
            valid = valid_string_array(json, value, 32U,
                romx_json_string_equals(json, key, "genre") ? 64U : 32U);
        } else if (romx_json_string_equals(json, key, "users")) {
            int64_t users = 0;
            valid = romx_json_integer(json, value, &users) &&
                users >= 1 && users <= 255;
        } else if (romx_json_string_equals(json, key, "coop") ||
            romx_json_string_equals(json, key, "rumble") ||
            romx_json_string_equals(json, key, "analog")) {
            valid = valid_boolean(json, value);
        } else if (romx_json_string_equals(json, key, "dump_status")) {
            valid = string_is_one_of(json, value, dumps,
                sizeof(dumps) / sizeof(dumps[0]));
        } else if (romx_json_string_equals(json, key, "cover")) {
            valid = valid_cover_object(json, value);
        } else {
            valid = 0;
        }
        if (!valid) {
            return 0;
        }
        key = value;
    }

    return required_schema && required_name;
}

romx_result_t romx_validate_metadata_bytes(const uint8_t *bytes,
    size_t size, romx_error_t *error)
{
    romx_json_document_t json;
    size_t bad_offset = 0U;

    if (bytes == NULL || size == 0U) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "metadata bytes must not be empty");
    }
    if (size >= 3U && bytes[0] == UINT8_C(0xef) &&
        bytes[1] == UINT8_C(0xbb) && bytes[2] == UINT8_C(0xbf)) {
        return romx_error_set(error, ROMX_E_METADATA_UTF8, 0, UINT64_C(0),
            "metadata must not contain a UTF-8 BOM");
    }
    if (!romx_utf8_validate(bytes, size, &bad_offset)) {
        return romx_error_set(error, ROMX_E_METADATA_UTF8, 0,
            (uint64_t)bad_offset, "metadata contains invalid UTF-8");
    }
    if (!romx_json_parse(&json, bytes, size, &bad_offset)) {
        return romx_error_set(error, ROMX_E_METADATA_JSON, 0,
            (uint64_t)bad_offset, "metadata contains invalid JSON");
    }
    if (!valid_schema(&json)) {
        romx_json_destroy(&json);
        return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
            UINT64_C(0), "metadata does not conform to ROMX schema 0.2.0");
    }
    romx_json_destroy(&json);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_metadata_load_internal(
    const romx_reader_t *reader,
    romx_metadata_t **out_metadata,
    romx_error_t *error)
{
    romx_metadata_t *metadata;
    size_t bad_offset = 0U;
    romx_result_t result;

    if (out_metadata != NULL) {
        *out_metadata = NULL;
    }
    if (reader == NULL || out_metadata == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN,
            "reader and out_metadata must not be null");
    }
    if (reader->info.metadata.size == UINT64_C(0)) {
        return romx_error_set(error, ROMX_E_METADATA_ABSENT, 0,
            ROMX_OFFSET_UNKNOWN, "ROMX has no metadata region");
    }
    if (reader->info.metadata.size > reader->max_metadata_size ||
        reader->info.metadata.size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_METADATA_TOO_LARGE, 0,
            reader->info.metadata.offset,
            "metadata exceeds configured size limit");
    }

    metadata = (romx_metadata_t *)calloc(1U, sizeof(*metadata));
    if (metadata == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate metadata handle");
    }
    metadata->size = (size_t)reader->info.metadata.size;
    metadata->bytes = (uint8_t *)malloc(metadata->size);
    if (metadata->bytes == NULL) {
        free(metadata);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate metadata buffer");
    }
    result = romx_read_exact(reader, reader->info.metadata.offset,
        metadata->bytes, reader->info.metadata.size, error);
    if (result != ROMX_OK) {
        romx_metadata_close(metadata);
        return result;
    }
    if (metadata->size >= 3U && metadata->bytes[0] == UINT8_C(0xef) &&
        metadata->bytes[1] == UINT8_C(0xbb) &&
        metadata->bytes[2] == UINT8_C(0xbf)) {
        romx_metadata_close(metadata);
        return romx_error_set(error, ROMX_E_METADATA_UTF8, 0,
            reader->info.metadata.offset,
            "metadata must not contain a UTF-8 BOM");
    }
    if (!romx_utf8_validate(metadata->bytes, metadata->size, &bad_offset)) {
        romx_metadata_close(metadata);
        return romx_error_set(error, ROMX_E_METADATA_UTF8, 0,
            reader->info.metadata.offset + bad_offset,
            "metadata contains invalid UTF-8");
    }
    if (!romx_json_parse(&metadata->json, metadata->bytes,
        metadata->size, &bad_offset)) {
        romx_metadata_close(metadata);
        return romx_error_set(error, ROMX_E_METADATA_JSON, 0,
            reader->info.metadata.offset + bad_offset,
            "metadata contains invalid JSON");
    }
    if (!valid_schema(&metadata->json)) {
        romx_metadata_close(metadata);
        return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
            reader->info.metadata.offset,
            "metadata does not conform to ROMX schema 0.2.0");
    }

    *out_metadata = metadata;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_metadata_open(const romx_reader_t *reader,
    romx_metadata_t **out_metadata, romx_error_t *error)
{
    return romx_metadata_load_internal(reader, out_metadata, error);
}

void romx_metadata_close(romx_metadata_t *metadata)
{
    if (metadata != NULL) {
        romx_json_destroy(&metadata->json);
        free(metadata->bytes);
        free(metadata);
    }
}

romx_result_t romx_metadata_copy_json(const romx_metadata_t *metadata,
    void *buffer, uint64_t capacity, uint64_t *required_size,
    romx_error_t *error)
{
    if (metadata == NULL || required_size == NULL ||
        (buffer == NULL && capacity != 0U)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid metadata JSON copy arguments");
    }
    *required_size = (uint64_t)metadata->size;
    if (capacity < metadata->size || buffer == NULL) {
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN,
            "metadata JSON output buffer is too small");
    }
    memcpy(buffer, metadata->bytes, metadata->size);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_metadata_get_string(const romx_metadata_t *metadata,
    const char *key, char *buffer, uint64_t capacity,
    uint64_t *required_size, romx_error_t *error)
{
    int token;
    size_t required = 0U;

    if (metadata == NULL || key == NULL || required_size == NULL ||
        (buffer == NULL && capacity != 0U) ||
        capacity > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid metadata string arguments");
    }
    token = romx_json_object_value(&metadata->json, 0, key);
    if (token < 0 || metadata->json.tokens[token].type != ROMX_JSON_STRING) {
        return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
            ROMX_OFFSET_UNKNOWN,
            "metadata string field is absent or has the wrong type");
    }
    if (!romx_json_copy_string(&metadata->json, token, buffer,
        (size_t)capacity, &required)) {
        *required_size = (uint64_t)required;
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN,
            "metadata string output buffer is too small");
    }
    *required_size = (uint64_t)required;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_metadata_get_crc32(
    const romx_metadata_t *metadata,
    uint32_t *crc32,
    romx_error_t *error)
{
    char value[9];
    uint64_t required = 0U;
    uint32_t parsed = UINT32_C(0);
    size_t index;
    romx_result_t result;

    if (crc32 != NULL) {
        *crc32 = UINT32_C(0);
    }
    if (metadata == NULL || crc32 == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "metadata and CRC32 output must not be null");
    }
    result = romx_metadata_get_string(metadata, "crc32", value,
        sizeof(value), &required, error);
    if (result != ROMX_OK) {
        return result;
    }
    if (required != sizeof(value)) {
        return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
            ROMX_OFFSET_UNKNOWN, "metadata CRC32 must contain eight hexadecimal characters");
    }
    for (index = 0U; index < 8U; ++index) {
        uint32_t nibble;
        if (value[index] >= '0' && value[index] <= '9') {
            nibble = (uint32_t)(value[index] - '0');
        } else if (value[index] >= 'a' && value[index] <= 'f') {
            nibble = (uint32_t)(value[index] - 'a') + UINT32_C(10);
        } else if (value[index] >= 'A' && value[index] <= 'F') {
            nibble = (uint32_t)(value[index] - 'A') + UINT32_C(10);
        } else {
            return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
                ROMX_OFFSET_UNKNOWN, "metadata CRC32 is not hexadecimal");
        }
        parsed = (parsed << 4U) | nibble;
    }
    *crc32 = parsed;
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_metadata_get_value_json(const romx_metadata_t *metadata,
    const char *key, void *buffer, uint64_t capacity,
    uint64_t *required_size, romx_error_t *error)
{
    int token;
    size_t size;

    if (metadata == NULL || key == NULL || required_size == NULL ||
        (buffer == NULL && capacity != 0U)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid metadata value arguments");
    }
    token = romx_json_object_value(&metadata->json, 0, key);
    if (token < 0) {
        return romx_error_set(error, ROMX_E_METADATA_SCHEMA, 0,
            ROMX_OFFSET_UNKNOWN, "metadata field is absent");
    }
    size = metadata->json.tokens[token].end -
        metadata->json.tokens[token].start;
    *required_size = (uint64_t)size;
    if (buffer == NULL || capacity < size) {
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN,
            "metadata value output buffer is too small");
    }
    memcpy(buffer, metadata->bytes + metadata->json.tokens[token].start,
        size);
    romx_error_clear(error);
    return ROMX_OK;
}

romx_result_t romx_validate_metadata_internal(
    const romx_reader_t *reader,
    romx_validation_report_t *report,
    romx_error_t *detail_error)
{
    romx_metadata_t *metadata = NULL;
    romx_result_t result = romx_metadata_load_internal(
        reader, &metadata, detail_error);

    if (result == ROMX_E_METADATA_ABSENT) {
        report->metadata = ROMX_STATUS_ABSENT;
        return result;
    }
    if (result != ROMX_OK) {
        report->metadata = ROMX_STATUS_INVALID;
        return result;
    }

    report->metadata = ROMX_STATUS_VALID;
    romx_metadata_close(metadata);
    return ROMX_OK;
}
