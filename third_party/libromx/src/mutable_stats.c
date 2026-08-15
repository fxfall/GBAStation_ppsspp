#include "romx_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct stats_memory_source {
    const uint8_t *bytes;
    uint64_t size;
} stats_memory_source_t;

static romx_result_t stats_memory_size(void *user_data, uint64_t *size,
    romx_error_t *error)
{
    stats_memory_source_t *source = (stats_memory_source_t *)user_data;
    (void)error;
    *size = source->size;
    return ROMX_OK;
}

static romx_result_t stats_memory_read(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    stats_memory_source_t *source = (stats_memory_source_t *)user_data;
    uint64_t count;
    (void)error;
    if (offset > source->size) return ROMX_E_RANGE;
    count = source->size - offset;
    if (count > size) count = size;
    if (count != UINT64_C(0))
        memcpy(buffer, source->bytes + (size_t)offset, (size_t)count);
    *bytes_read = count;
    return ROMX_OK;
}

static int object_has_unique_keys(const romx_json_document_t *json,
    int object)
{
    int first = -1;
    for (;;) {
        int first_value;
        int second = -1;
        first = romx_json_next_direct_child(json, object, first);
        if (first < 0) return 1;
        first_value = romx_json_next_direct_child(json, object, first);
        if (first_value < 0) return 0;
        for (;;) {
            int second_value;
            second = romx_json_next_direct_child(json, object, second);
            if (second < 0) break;
            second_value = romx_json_next_direct_child(json, object, second);
            if (second_value < 0) return 0;
            if (second != first && romx_json_strings_equal(json, first, second))
                return 0;
            second = second_value;
        }
        first = first_value;
    }
}

static int json_uint53(const romx_json_document_t *json, int token,
    uint64_t *value)
{
    int64_t parsed;
    if (!romx_json_integer(json, token, &parsed) || parsed < 0 ||
        (uint64_t)parsed > ROMX_MUTABLE_STATS_MAX_SAFE_INTEGER) return 0;
    *value = (uint64_t)parsed;
    return 1;
}

static int json_boolean(const romx_json_document_t *json, int token,
    uint32_t *value)
{
    if (token < 0 || (size_t)token >= json->token_count) return 0;
    if (json->tokens[token].type == ROMX_JSON_TRUE) {
        *value = UINT32_C(1);
        return 1;
    }
    if (json->tokens[token].type == ROMX_JSON_FALSE) {
        *value = UINT32_C(0);
        return 1;
    }
    return 0;
}

static int parse_achievements(const romx_json_document_t *json, int object,
    romx_mutable_stats_t *stats)
{
    int item = -1;
    int have_unlocked = 0;
    int have_total = 0;
    if (object < 0 || (size_t)object >= json->token_count ||
        json->tokens[object].type != ROMX_JSON_OBJECT ||
        !object_has_unique_keys(json, object)) return 0;
    for (;;) {
        int value;
        item = romx_json_next_direct_child(json, object, item);
        if (item < 0) break;
        value = romx_json_next_direct_child(json, object, item);
        if (value < 0) return 0;
        if (romx_json_string_equals(json, item, "unlocked")) {
            if (!json_uint53(json, value, &stats->achievements_unlocked)) return 0;
            have_unlocked = 1;
        } else if (romx_json_string_equals(json, item, "total")) {
            if (!json_uint53(json, value, &stats->achievements_total)) return 0;
            have_total = 1;
        } else if (romx_json_string_equals(json, item, "hardcore_unlocked")) {
            if (!json_uint53(json, value,
                    &stats->achievements_hardcore_unlocked)) return 0;
            stats->flags |= ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED;
        } else return 0;
        item = value;
    }
    if (!have_unlocked || !have_total ||
        stats->achievements_unlocked > stats->achievements_total ||
        ((stats->flags & ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED) &&
         stats->achievements_hardcore_unlocked >
            stats->achievements_unlocked)) return 0;
    stats->flags |= ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS;
    return 1;
}

romx_result_t romx_mutable_stats_parse_json(const void *json_bytes,
    uint64_t json_size, romx_mutable_stats_t *stats, romx_error_t *error)
{
    romx_json_document_t json;
    romx_mutable_stats_t parsed = ROMX_MUTABLE_STATS_INIT;
    size_t bad_offset = 0U;
    int item = -1;
    int have_schema = 0;
    int have_version = 0;

    if (json_bytes == NULL || stats == NULL ||
        stats->struct_size < (uint32_t)sizeof(*stats) ||
        json_size == UINT64_C(0) ||
        json_size > ROMX_MUTABLE_STATS_MAX_JSON_SIZE ||
        json_size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable STATS JSON arguments");
    }
    memset(&json, 0, sizeof(json));
    if (!romx_utf8_validate((const uint8_t *)json_bytes, (size_t)json_size,
            &bad_offset) ||
        !romx_json_parse(&json, (const uint8_t *)json_bytes,
            (size_t)json_size, &bad_offset) ||
        json.token_count == 0U || json.tokens[0].type != ROMX_JSON_OBJECT ||
        !object_has_unique_keys(&json, 0)) {
        romx_json_destroy(&json);
        return romx_error_set(error, ROMX_E_MUTABLE_STATS, 0,
            (uint64_t)bad_offset, "mutable STATS is not strict JSON");
    }

    for (;;) {
        int value;
        item = romx_json_next_direct_child(&json, 0, item);
        if (item < 0) break;
        value = romx_json_next_direct_child(&json, 0, item);
        if (value < 0) goto schema_error;
        if (romx_json_string_equals(&json, item, "schema")) {
            if (json.tokens[value].type != ROMX_JSON_STRING ||
                !romx_json_string_equals(&json, value, "romx.stats"))
                goto schema_error;
            have_schema = 1;
        } else if (romx_json_string_equals(&json, item, "version")) {
            int64_t version;
            if (!romx_json_integer(&json, value, &version) ||
                version != (int64_t)ROMX_MUTABLE_STATS_VERSION)
                goto schema_error;
            have_version = 1;
        } else if (romx_json_string_equals(&json, item, "play_time_seconds")) {
            if (!json_uint53(&json, value, &parsed.play_time_seconds))
                goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_PLAY_TIME;
        } else if (romx_json_string_equals(&json, item, "launch_count")) {
            if (!json_uint53(&json, value, &parsed.launch_count))
                goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT;
        } else if (romx_json_string_equals(&json, item,
                "first_played_unix_seconds")) {
            if (!json_uint53(&json, value,
                    &parsed.first_played_unix_seconds)) goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED;
        } else if (romx_json_string_equals(&json, item,
                "last_played_unix_seconds")) {
            if (!json_uint53(&json, value,
                    &parsed.last_played_unix_seconds)) goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_LAST_PLAYED;
        } else if (romx_json_string_equals(&json, item, "favorite")) {
            if (!json_boolean(&json, value, &parsed.favorite)) goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_FAVORITE;
        } else if (romx_json_string_equals(&json, item, "completed")) {
            if (!json_boolean(&json, value, &parsed.completed)) goto schema_error;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_COMPLETED;
        } else if (romx_json_string_equals(&json, item,
                "completion_percent")) {
            uint64_t percent;
            if (!json_uint53(&json, value, &percent) || percent > UINT64_C(100))
                goto schema_error;
            parsed.completion_percent = (uint32_t)percent;
            parsed.flags |= ROMX_MUTABLE_STATS_HAS_COMPLETION_PERCENT;
        } else if (romx_json_string_equals(&json, item, "achievements")) {
            if (!parse_achievements(&json, value, &parsed)) goto schema_error;
        } else goto schema_error;
        item = value;
    }
    if (!have_schema || !have_version ||
        ((parsed.flags & ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED) &&
         (parsed.flags & ROMX_MUTABLE_STATS_HAS_LAST_PLAYED) &&
         parsed.first_played_unix_seconds > parsed.last_played_unix_seconds))
        goto schema_error;

    romx_json_destroy(&json);
    *stats = parsed;
    romx_error_clear(error);
    return ROMX_OK;

schema_error:
    romx_json_destroy(&json);
    return romx_error_set(error, ROMX_E_MUTABLE_STATS, 0,
        ROMX_OFFSET_UNKNOWN, "mutable STATS JSON does not match schema version 1");
}

static int append_text(char *output, size_t capacity, size_t *position,
    const char *text)
{
    size_t size = strlen(text);
    if (*position > capacity || size > capacity - *position) return 0;
    memcpy(output + *position, text, size);
    *position += size;
    return 1;
}

static int append_uint(char *output, size_t capacity, size_t *position,
    uint64_t value)
{
    int count;
    if (*position >= capacity) return 0;
    count = snprintf(output + *position, capacity - *position,
        "%" PRIu64, value);
    if (count < 0 || (size_t)count >= capacity - *position) return 0;
    *position += (size_t)count;
    return 1;
}

static int stats_values_valid(const romx_mutable_stats_t *stats)
{
    const uint64_t limit = ROMX_MUTABLE_STATS_MAX_SAFE_INTEGER;
    if (stats == NULL || stats->struct_size < (uint32_t)sizeof(*stats) ||
        (stats->flags & ~ROMX_MUTABLE_STATS_FLAGS_MASK) != 0U ||
        stats->reserved != UINT32_C(0) ||
        ((stats->flags & ROMX_MUTABLE_STATS_HAS_FAVORITE) &&
            stats->favorite > UINT32_C(1)) ||
        ((stats->flags & ROMX_MUTABLE_STATS_HAS_COMPLETED) &&
            stats->completed > UINT32_C(1)) ||
        ((stats->flags & ROMX_MUTABLE_STATS_HAS_COMPLETION_PERCENT) &&
            stats->completion_percent > UINT32_C(100)) ||
        ((stats->flags & ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED) != 0U &&
            (stats->flags & ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS) == 0U))
        return 0;
    if (stats->play_time_seconds > limit || stats->launch_count > limit ||
        stats->first_played_unix_seconds > limit ||
        stats->last_played_unix_seconds > limit ||
        stats->achievements_unlocked > limit ||
        stats->achievements_total > limit ||
        stats->achievements_hardcore_unlocked > limit) return 0;
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED) &&
        (stats->flags & ROMX_MUTABLE_STATS_HAS_LAST_PLAYED) &&
        stats->first_played_unix_seconds > stats->last_played_unix_seconds)
        return 0;
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS) &&
        stats->achievements_unlocked > stats->achievements_total) return 0;
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED) &&
        stats->achievements_hardcore_unlocked > stats->achievements_unlocked)
        return 0;
    return 1;
}

romx_result_t romx_mutable_stats_serialize_json(
    const romx_mutable_stats_t *stats, void *buffer, uint64_t capacity,
    uint64_t *required_size, romx_error_t *error)
{
    char json[ROMX_MUTABLE_STATS_MAX_JSON_SIZE];
    size_t position = 0U;
#define APPEND_FIELD(flag, name, value) do { if ((stats->flags & (flag)) != 0U) { if (!append_text(json, sizeof(json), &position, ",\"" name "\":") || !append_uint(json, sizeof(json), &position, (value))) goto too_large; } } while (0)

    if (required_size == NULL || (buffer == NULL && capacity != 0U) ||
        capacity > (uint64_t)SIZE_MAX || !stats_values_valid(stats)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable STATS values");
    }
    if (!append_text(json, sizeof(json), &position,
            "{\"schema\":\"romx.stats\",\"version\":1")) goto too_large;
    APPEND_FIELD(ROMX_MUTABLE_STATS_HAS_PLAY_TIME,
        "play_time_seconds", stats->play_time_seconds);
    APPEND_FIELD(ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT,
        "launch_count", stats->launch_count);
    APPEND_FIELD(ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED,
        "first_played_unix_seconds", stats->first_played_unix_seconds);
    APPEND_FIELD(ROMX_MUTABLE_STATS_HAS_LAST_PLAYED,
        "last_played_unix_seconds", stats->last_played_unix_seconds);
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_FAVORITE) != 0U &&
        !append_text(json, sizeof(json), &position,
            stats->favorite ? ",\"favorite\":true" : ",\"favorite\":false"))
        goto too_large;
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_COMPLETED) != 0U &&
        !append_text(json, sizeof(json), &position,
            stats->completed ? ",\"completed\":true" : ",\"completed\":false"))
        goto too_large;
    APPEND_FIELD(ROMX_MUTABLE_STATS_HAS_COMPLETION_PERCENT,
        "completion_percent", stats->completion_percent);
    if ((stats->flags & ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS) != 0U) {
        if (!append_text(json, sizeof(json), &position,
                ",\"achievements\":{\"unlocked\":") ||
            !append_uint(json, sizeof(json), &position,
                stats->achievements_unlocked) ||
            !append_text(json, sizeof(json), &position, ",\"total\":") ||
            !append_uint(json, sizeof(json), &position,
                stats->achievements_total)) goto too_large;
        if ((stats->flags & ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED) != 0U &&
            (!append_text(json, sizeof(json), &position,
                ",\"hardcore_unlocked\":") ||
             !append_uint(json, sizeof(json), &position,
                stats->achievements_hardcore_unlocked))) goto too_large;
        if (!append_text(json, sizeof(json), &position, "}")) goto too_large;
    }
    if (!append_text(json, sizeof(json), &position, "}")) goto too_large;
    *required_size = (uint64_t)position;
    if (buffer == NULL || capacity < (uint64_t)position) {
        return romx_error_set(error, ROMX_E_BUFFER_TOO_SMALL, 0,
            ROMX_OFFSET_UNKNOWN, "mutable STATS output buffer is too small");
    }
    memcpy(buffer, json, position);
    romx_error_clear(error);
    return ROMX_OK;

too_large:
    *required_size = UINT64_C(0);
    return romx_error_set(error, ROMX_E_MUTABLE_STATS, 0,
        ROMX_OFFSET_UNKNOWN, "mutable STATS JSON exceeds the profile limit");
#undef APPEND_FIELD
}

romx_result_t romx_mutable_stats_read(const romx_reader_t *reader,
    const char *key, romx_mutable_stats_t *stats, romx_error_t *error)
{
    romx_mutable_file_t *file = NULL;
    uint8_t *json = NULL;
    uint64_t size = UINT64_C(0);
    uint64_t read = UINT64_C(0);
    romx_result_t result;
    if (reader == NULL || key == NULL || stats == NULL)
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable STATS read arguments");
    result = romx_mutable_file_open(reader, ROMX_MUTABLE_NAMESPACE_STATS,
        key, &file, error);
    if (result != ROMX_OK) return result;
    result = romx_mutable_file_get_size(file, &size, error);
    if (result != ROMX_OK || size == UINT64_C(0) ||
        size > ROMX_MUTABLE_STATS_MAX_JSON_SIZE) {
        romx_mutable_file_close(file);
        return result != ROMX_OK ? result : romx_error_set(error,
            ROMX_E_MUTABLE_STATS, 0, ROMX_OFFSET_UNKNOWN,
            "mutable STATS object exceeds the profile limit");
    }
    json = (uint8_t *)malloc((size_t)size);
    if (json == NULL) {
        romx_mutable_file_close(file);
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable STATS JSON");
    }
    result = romx_mutable_file_read(file, json, size, &read, error);
    romx_mutable_file_close(file);
    if (result == ROMX_OK && read != size)
        result = romx_error_set(error, ROMX_E_TRUNCATED, 0,
            ROMX_OFFSET_UNKNOWN, "mutable STATS object is truncated");
    if (result == ROMX_OK)
        result = romx_mutable_stats_parse_json(json, size, stats, error);
    free(json);
    return result;
}

romx_result_t romx_mutable_stats_write_path(const char *romx_path,
    const char *key, const romx_mutable_stats_t *stats,
    const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written, romx_error_t *error)
{
    uint8_t json[ROMX_MUTABLE_STATS_MAX_JSON_SIZE];
    uint64_t size = UINT64_C(0);
    stats_memory_source_t source;
    romx_io_t io = ROMX_IO_INIT;
    romx_result_t result = romx_mutable_stats_serialize_json(stats, json,
        sizeof(json), &size, error);
    if (result != ROMX_OK) return result;
    source.bytes = json;
    source.size = size;
    io.user_data = &source;
    io.get_size = stats_memory_size;
    io.read_at = stats_memory_read;
    return romx_mutable_write_io_path(romx_path,
        ROMX_MUTABLE_NAMESPACE_STATS, key, &io, options, written, error);
}
