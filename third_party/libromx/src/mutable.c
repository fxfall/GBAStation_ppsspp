#include "romx_internal.h"

#include <stdlib.h>
#include <string.h>

#define ROMX_MUTABLE_HEADER_SIZE UINT64_C(4096)
#define ROMX_MUTABLE_ENTRY_SIZE UINT64_C(512)
#define ROMX_MUTABLE_STATE_ACTIVE UINT16_C(1)
#define ROMX_MUTABLE_STATE_WRITING UINT16_C(2)
#define ROMX_MUTABLE_STATE_DELETING UINT16_C(3)

struct romx_mutable_file {
    const romx_reader_t *reader;
    uint64_t absolute_offset;
    uint64_t size;
    uint64_t position;
};

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

static uint32_t crc32_with_zero_field(uint8_t *bytes, size_t size,
    size_t field_offset)
{
    uint8_t saved[4];
    uint32_t crc;
    memcpy(saved, bytes + field_offset, sizeof(saved));
    memset(bytes + field_offset, 0, sizeof(saved));
    crc = romx_crc32_begin();
    crc = romx_crc32_update(crc, bytes, size);
    crc = romx_crc32_finish(crc);
    memcpy(bytes + field_offset, saved, sizeof(saved));
    return crc;
}

static int key_is_valid(const uint8_t *key, size_t size,
    romx_mutable_namespace_t object_namespace)
{
    size_t component = 0U;
    size_t index;
    size_t bad_offset = 0U;
    int has_private_separator = 0;

    if (size == 0U || key[0] == (uint8_t)'/' ||
        key[size - 1U] == (uint8_t)'/' ||
        !romx_utf8_validate(key, size, &bad_offset)) return 0;
    for (index = 0U; index <= size; ++index) {
        if (index < size && key[index] != (uint8_t)'/') {
            if (key[index] == UINT8_C(0) || key[index] == (uint8_t)'\\') return 0;
            continue;
        }
        if (index == component ||
            (index - component == 1U && key[component] == (uint8_t)'.') ||
            (index - component == 2U && key[component] == (uint8_t)'.' &&
                key[component + 1U] == (uint8_t)'.')) return 0;
        if (object_namespace == ROMX_MUTABLE_NAMESPACE_PRIVATE &&
            component == 0U && index > 0U && index < size) {
            has_private_separator = 1;
        }
        component = index + 1U;
    }
    return object_namespace != ROMX_MUTABLE_NAMESPACE_PRIVATE ||
        has_private_separator;
}

static int ascii_fold_equal(const char *first, const char *second)
{
    size_t index = 0U;
    for (;;) {
        unsigned char a = (unsigned char)first[index];
        unsigned char b = (unsigned char)second[index];
        if (a >= (unsigned char)'A' && a <= (unsigned char)'Z') a = (unsigned char)(a + 32U);
        if (b >= (unsigned char)'A' && b <= (unsigned char)'Z') b = (unsigned char)(b + 32U);
        if (a != b) return 0;
        if (a == 0U) return 1;
        ++index;
    }
}

static int extents_overlap(const romx_mutable_object_info_t *first,
    const romx_mutable_object_info_t *second)
{
    return first->data_offset < second->data_offset + second->data_capacity &&
        second->data_offset < first->data_offset + first->data_capacity;
}

static void mutable_mark_invalid(romx_reader_t *reader)
{
    free(reader->mutable_slots);
    reader->mutable_slots = NULL;
    reader->mutable_slot_count = UINT32_C(0);
    reader->mutable_status = ROMX_MUTABLE_INVALID;
}

romx_result_t romx_parse_mutable(romx_reader_t *reader, romx_error_t *error)
{
    uint8_t header[4096];
    uint32_t entry_capacity;
    uint64_t directory_size;
    uint64_t data_area_offset;
    uint64_t data_area_size;
    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t slot_index;
    romx_result_t result;

    if (reader == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader must not be null");
    }
    reader->mutable_status = ROMX_MUTABLE_ABSENT;
    if (reader->info.mutable_region.size == UINT64_C(0)) return ROMX_OK;

    result = romx_read_exact(reader, reader->info.mutable_region.offset,
        header, sizeof(header), error);
    if (result != ROMX_OK) {
        mutable_mark_invalid(reader);
        romx_error_clear(error);
        return ROMX_OK;
    }
    expected_crc = read_le32(header + 0x34U);
    actual_crc = crc32_with_zero_field(header, sizeof(header), 0x34U);
    entry_capacity = read_le32(header + 0x0CU);
    directory_size = read_le64(header + 0x18U);
    data_area_offset = read_le64(header + 0x20U);
    data_area_size = read_le64(header + 0x28U);
    if (memcmp(header, "RMUT", 4U) != 0 ||
        read_le16(header + 0x04U) != UINT16_C(1) ||
        read_le16(header + 0x06U) != UINT16_C(4096) ||
        read_le32(header + 0x08U) != UINT32_C(512) ||
        entry_capacity < UINT32_C(8) || entry_capacity % UINT32_C(8) != 0U ||
        read_le64(header + 0x10U) != ROMX_MUTABLE_HEADER_SIZE ||
        directory_size != (uint64_t)entry_capacity * ROMX_MUTABLE_ENTRY_SIZE ||
        data_area_offset != ROMX_MUTABLE_HEADER_SIZE + directory_size ||
        data_area_offset % UINT64_C(4096) != UINT64_C(0) ||
        data_area_offset >= reader->info.mutable_region.size ||
        data_area_size != reader->info.mutable_region.size - data_area_offset ||
        read_le32(header + 0x30U) != UINT32_C(0) ||
        expected_crc != actual_crc || !bytes_are_zero(header + 0x38U, 4040U)) {
        mutable_mark_invalid(reader);
        romx_error_clear(error);
        return ROMX_OK;
    }
    reader->mutable_slots = (struct romx_mutable_slot *)calloc(
        (size_t)entry_capacity, sizeof(*reader->mutable_slots));
    if (reader->mutable_slots == NULL) {
        mutable_mark_invalid(reader);
        romx_error_clear(error);
        return ROMX_OK;
    }
    reader->mutable_slot_count = entry_capacity;
    reader->mutable_status = ROMX_MUTABLE_VALID;

    for (slot_index = 0U; slot_index < entry_capacity; ++slot_index) {
        uint8_t stored[512];
        struct romx_mutable_slot *slot = &reader->mutable_slots[slot_index];
        romx_mutable_object_info_t *object = &slot->object;
        uint32_t key_size;
        uint64_t entry_offset = reader->info.mutable_region.offset +
            ROMX_MUTABLE_HEADER_SIZE +
            (uint64_t)slot_index * ROMX_MUTABLE_ENTRY_SIZE;

        result = romx_read_exact(reader, entry_offset, stored, sizeof(stored), error);
        if (result != ROMX_OK) {
            mutable_mark_invalid(reader);
            romx_error_clear(error);
            return ROMX_OK;
        }
        if (bytes_are_zero(stored, sizeof(stored))) continue;
        object->struct_size = (uint32_t)sizeof(*object);
        object->slot_index = slot_index;
        slot->state = read_le16(stored + 0x04U);
        object->object_namespace = read_le16(stored + 0x06U);
        key_size = read_le32(stored + 0x0CU);
        object->data_offset = read_le64(stored + 0x10U);
        object->data_capacity = read_le64(stored + 0x18U);
        object->data_size = read_le64(stored + 0x20U);
        object->generation = read_le64(stored + 0x28U);
        object->modified_unix_seconds = read_le64(stored + 0x30U);
        object->data_crc32 = read_le32(stored + 0x38U);
        object->key_size = key_size;

        if (memcmp(stored, "MENT", 4U) != 0 ||
            slot->state < ROMX_MUTABLE_STATE_ACTIVE ||
            slot->state > ROMX_MUTABLE_STATE_DELETING ||
            object->object_namespace < ROMX_MUTABLE_NAMESPACE_SAVE ||
            object->object_namespace > ROMX_MUTABLE_NAMESPACE_PRIVATE ||
            read_le32(stored + 0x08U) != UINT32_C(0) ||
            key_size == UINT32_C(0) || key_size > ROMX_MUTABLE_KEY_CAPACITY ||
            object->data_offset < data_area_offset ||
            object->data_offset % UINT64_C(64) != UINT64_C(0) ||
            object->data_capacity == UINT64_C(0) ||
            object->data_offset > reader->info.mutable_region.size ||
            object->data_capacity >
                reader->info.mutable_region.size - object->data_offset ||
            object->data_size > object->data_capacity ||
            (object->data_size == UINT64_C(0) &&
                object->data_crc32 != UINT32_C(0)) ||
            object->generation == UINT64_C(0) ||
            !bytes_are_zero(stored + 0x40U + key_size,
                ROMX_MUTABLE_KEY_CAPACITY - key_size) ||
            !key_is_valid(stored + 0x40U, key_size,
                object->object_namespace) ||
            read_le32(stored + 0x3CU) !=
                crc32_with_zero_field(stored, sizeof(stored), 0x3CU)) {
            reader->mutable_status = ROMX_MUTABLE_DEGRADED;
            continue;
        }
        memcpy(object->key, stored + 0x40U, key_size);
        object->key[key_size] = '\0';
        slot->usable = 1;
    }

    for (slot_index = 0U; slot_index < entry_capacity; ++slot_index) {
        uint32_t other;
        struct romx_mutable_slot *first = &reader->mutable_slots[slot_index];
        if (!first->usable) continue;
        for (other = slot_index + 1U; other < entry_capacity; ++other) {
            struct romx_mutable_slot *second = &reader->mutable_slots[other];
            if (!second->usable) continue;
            if ((first->object.object_namespace == second->object.object_namespace &&
                    ascii_fold_equal(first->object.key, second->object.key)) ||
                extents_overlap(&first->object, &second->object)) {
                first->usable = 0;
                second->usable = 0;
                reader->mutable_status = ROMX_MUTABLE_DEGRADED;
            }
        }
    }
    romx_error_clear(error);
    return ROMX_OK;
}

static romx_result_t require_mutable(const romx_reader_t *reader,
    romx_error_t *error)
{
    if (reader->mutable_status == ROMX_MUTABLE_ABSENT) {
        return romx_error_set(error, ROMX_E_MUTABLE_ABSENT, 0,
            ROMX_OFFSET_UNKNOWN, "ROMX has no mutable region");
    }
    if (reader->mutable_status == ROMX_MUTABLE_INVALID) {
        return romx_error_set(error, ROMX_E_MUTABLE_HEADER, 0,
            reader->info.mutable_region.offset, "ROMX mutable header is invalid");
    }
    return ROMX_OK;
}

static romx_result_t validate_slot_data(const romx_reader_t *reader,
    const struct romx_mutable_slot *slot, romx_error_t *error)
{
    uint8_t *buffer;
    uint64_t position = UINT64_C(0);
    uint32_t crc = romx_crc32_begin();
    romx_result_t result = ROMX_OK;

    buffer = (uint8_t *)malloc(reader->io_chunk_size);
    if (buffer == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable CRC32 buffer");
    }
    while (position < slot->object.data_size) {
        uint64_t count = slot->object.data_size - position;
        if (count > reader->io_chunk_size) count = reader->io_chunk_size;
        result = romx_read_exact(reader, reader->info.mutable_region.offset +
            slot->object.data_offset + position, buffer, count, error);
        if (result != ROMX_OK) break;
        crc = romx_crc32_update(crc, buffer, (size_t)count);
        position += count;
    }
    free(buffer);
    if (result != ROMX_OK) return result;
    crc = romx_crc32_finish(crc);
    if (crc != slot->object.data_crc32) {
        return romx_error_set(error, ROMX_E_MUTABLE_DATA_CRC, 0,
            reader->info.mutable_region.offset + slot->object.data_offset,
            "mutable object data CRC32 mismatch");
    }
    return ROMX_OK;
}

static romx_result_t copy_object(const struct romx_mutable_slot *slot,
    romx_mutable_object_info_t *object, romx_error_t *error)
{
    uint32_t supplied_size;
    if (object == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "mutable object output must not be null");
    }
    supplied_size = object->struct_size;
    if (supplied_size < sizeof(*object)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "mutable object structure is too small");
    }
    memcpy(object, &slot->object, sizeof(*object));
    object->struct_size = supplied_size;
    return ROMX_OK;
}

romx_result_t romx_reader_get_mutable_status(const romx_reader_t *reader,
    romx_mutable_status_t *status, romx_error_t *error)
{
    romx_error_clear(error);
    if (reader == NULL || status == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and mutable status must not be null");
    }
    *status = reader->mutable_status;
    return ROMX_OK;
}

romx_result_t romx_reader_get_mutable_object_count(const romx_reader_t *reader,
    uint32_t *count, romx_error_t *error)
{
    uint32_t index;
    romx_result_t result;
    romx_error_clear(error);
    if (reader == NULL || count == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and mutable count must not be null");
    }
    *count = UINT32_C(0);
    if (reader->mutable_status == ROMX_MUTABLE_ABSENT) return ROMX_OK;
    result = require_mutable(reader, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (!slot->usable || slot->state != ROMX_MUTABLE_STATE_ACTIVE) continue;
        result = validate_slot_data(reader, slot, error);
        if (result != ROMX_OK) return result;
        ++*count;
    }
    return ROMX_OK;
}

romx_result_t romx_reader_get_mutable_object(const romx_reader_t *reader,
    uint32_t active_index, romx_mutable_object_info_t *object,
    romx_error_t *error)
{
    uint32_t index;
    uint32_t current = UINT32_C(0);
    romx_result_t result;
    romx_error_clear(error);
    if (reader == NULL || object == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and mutable object must not be null");
    }
    result = require_mutable(reader, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (!slot->usable || slot->state != ROMX_MUTABLE_STATE_ACTIVE) continue;
        result = validate_slot_data(reader, slot, error);
        if (result != ROMX_OK) return result;
        if (current++ == active_index) return copy_object(slot, object, error);
    }
    return romx_error_set(error, ROMX_E_MUTABLE_ENTRY, 0,
        ROMX_OFFSET_UNKNOWN, "mutable object index is out of range");
}

romx_result_t romx_reader_find_mutable_object(const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace, const char *key,
    romx_mutable_object_info_t *object, romx_error_t *error)
{
    uint32_t index;
    romx_result_t result;
    romx_error_clear(error);
    if (reader == NULL || key == NULL || *key == '\0' || object == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable object lookup arguments");
    }
    result = require_mutable(reader, error);
    if (result != ROMX_OK) return result;
    for (index = 0U; index < reader->mutable_slot_count; ++index) {
        const struct romx_mutable_slot *slot = &reader->mutable_slots[index];
        if (!slot->usable || slot->state != ROMX_MUTABLE_STATE_ACTIVE ||
            slot->object.object_namespace != object_namespace ||
            !ascii_fold_equal(slot->object.key, key)) continue;
        result = validate_slot_data(reader, slot, error);
        if (result != ROMX_OK) return result;
        return copy_object(slot, object, error);
    }
    return romx_error_set(error, ROMX_E_MUTABLE_ENTRY, 0,
        ROMX_OFFSET_UNKNOWN, "mutable object is not present");
}

romx_result_t romx_mutable_file_open(const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace, const char *key,
    romx_mutable_file_t **out_file, romx_error_t *error)
{
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_mutable_file_t *file;
    romx_result_t result;
    romx_error_clear(error);
    if (out_file != NULL) *out_file = NULL;
    if (reader == NULL || out_file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and mutable file output must not be null");
    }
    result = romx_reader_find_mutable_object(reader, object_namespace,
        key, &object, error);
    if (result != ROMX_OK) return result;
    file = (romx_mutable_file_t *)calloc(1U, sizeof(*file));
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate mutable file cursor");
    }
    file->reader = reader;
    file->absolute_offset = reader->info.mutable_region.offset + object.data_offset;
    file->size = object.data_size;
    *out_file = file;
    return ROMX_OK;
}

romx_result_t romx_mutable_file_get_size(const romx_mutable_file_t *file,
    uint64_t *size, romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || size == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "mutable file and size must not be null");
    }
    *size = file->size;
    return ROMX_OK;
}

romx_result_t romx_mutable_file_tell(const romx_mutable_file_t *file,
    uint64_t *position, romx_error_t *error)
{
    romx_error_clear(error);
    if (file == NULL || position == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "mutable file and position must not be null");
    }
    *position = file->position;
    return ROMX_OK;
}

static romx_result_t add_seek_offset(uint64_t base, int64_t offset,
    uint64_t *target, romx_error_t *error)
{
    uint64_t magnitude;
    if (offset >= 0) {
        magnitude = (uint64_t)offset;
        if (base > UINT64_MAX - magnitude) {
            return romx_error_set(error, ROMX_E_RANGE, 0,
                ROMX_OFFSET_UNKNOWN, "mutable file seek overflows");
        }
        *target = base + magnitude;
        return ROMX_OK;
    }
    magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
    if (magnitude > base) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            ROMX_OFFSET_UNKNOWN, "mutable file seek precedes its start");
    }
    *target = base - magnitude;
    return ROMX_OK;
}

romx_result_t romx_mutable_file_seek(romx_mutable_file_t *file,
    int64_t offset, romx_payload_seek_position_t origin,
    uint64_t *new_position, romx_error_t *error)
{
    uint64_t base;
    uint64_t target;
    romx_result_t result;
    romx_error_clear(error);
    if (file == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "mutable file must not be null");
    }
    switch (origin) {
    case ROMX_PAYLOAD_SEEK_START: base = UINT64_C(0); break;
    case ROMX_PAYLOAD_SEEK_CURRENT: base = file->position; break;
    case ROMX_PAYLOAD_SEEK_END: base = file->size; break;
    default:
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "unknown mutable file seek origin");
    }
    result = add_seek_offset(base, offset, &target, error);
    if (result != ROMX_OK) return result;
    file->position = target;
    if (new_position != NULL) *new_position = target;
    return ROMX_OK;
}

romx_result_t romx_mutable_file_read(romx_mutable_file_t *file,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    uint64_t count;
    romx_result_t result;
    romx_error_clear(error);
    if (bytes_read != NULL) *bytes_read = UINT64_C(0);
    if (file == NULL || bytes_read == NULL ||
        (buffer == NULL && size != UINT64_C(0)) ||
        size > (uint64_t)SIZE_MAX) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid mutable file read arguments");
    }
    if (size == UINT64_C(0) || file->position >= file->size) return ROMX_OK;
    count = file->size - file->position;
    if (count > size) count = size;
    result = file->reader->io.read_at(file->reader->io.user_data,
        file->absolute_offset + file->position, buffer, count,
        bytes_read, error);
    if (result != ROMX_OK) return result;
    if (*bytes_read != count) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            file->absolute_offset + file->position + *bytes_read,
            "mutable object read was truncated");
    }
    file->position += count;
    return ROMX_OK;
}

void romx_mutable_file_close(romx_mutable_file_t *file)
{
    free(file);
}
