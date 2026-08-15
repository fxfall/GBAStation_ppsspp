#include "romx_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct romx_range {
    uint64_t start;
    uint64_t end;
} romx_range_t;

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
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

static int path_is_valid(const uint8_t *path, size_t size)
{
    size_t index;
    size_t component_start = 0U;
    size_t bad_offset = 0U;

    if (size == 0U || path[0] == (uint8_t)'/' || path[size - 1U] == (uint8_t)'/' ||
        !romx_utf8_validate(path, size, &bad_offset)) {
        return 0;
    }
    for (index = 0U; index <= size; ++index) {
        if (index < size && path[index] != (uint8_t)'/') {
            if (path[index] == UINT8_C(0) || path[index] == (uint8_t)'\\') return 0;
            continue;
        }
        if (index == component_start ||
            (index - component_start == 1U && path[component_start] == (uint8_t)'.') ||
            (index - component_start == 2U && path[component_start] == (uint8_t)'.' &&
             path[component_start + 1U] == (uint8_t)'.')) {
            return 0;
        }
        component_start = index + 1U;
    }
    return 1;
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

static int range_compare(const void *left, const void *right)
{
    const romx_range_t *a = (const romx_range_t *)left;
    const romx_range_t *b = (const romx_range_t *)right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static romx_result_t region_is_zero(const romx_reader_t *reader,
    uint64_t offset, uint64_t size, romx_error_t *error)
{
    uint8_t buffer[4096];
    uint64_t position = UINT64_C(0);
    while (position < size) {
        uint64_t count = size - position;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        {
            romx_result_t result = romx_read_exact(reader, offset + position,
                buffer, count, error);
            if (result != ROMX_OK) return result;
        }
        if (!bytes_are_zero(buffer, (size_t)count)) {
            return romx_error_set(error, ROMX_E_INDEX, 0, offset + position,
                "unindexed payload or alignment bytes must be zero");
        }
        position += count;
    }
    return ROMX_OK;
}

romx_result_t romx_parse_ridx(romx_reader_t *reader, romx_error_t *error)
{
    uint8_t header[ROMX_RIDX_HEADER_SIZE];
    uint8_t *index_bytes = NULL;
    romx_range_t *ranges = NULL;
    uint32_t entry_count;
    uint64_t index_size;
    uint64_t available;
    uint64_t content_end;
    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t entrypoint_count = UINT32_C(0);
    uint32_t range_count = UINT32_C(0);
    uint32_t position;
    romx_result_t result;

    if (reader == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader must not be null");
    }
    result = romx_read_exact(reader, reader->info.payload.size,
        header, sizeof(header), error);
    if (result != ROMX_OK) return result;
    if (memcmp(header, "RIDX", 4U) != 0 || read_le16(header + 0x04U) != UINT16_C(1) ||
        read_le16(header + 0x06U) != ROMX_RIDX_HEADER_SIZE ||
        read_le32(header + 0x0CU) != ROMX_RIDX_ENTRY_SIZE ||
        read_le32(header + 0x10U) != UINT32_C(0) ||
        !bytes_are_zero(header + 0x18U, 40U)) {
        return romx_error_set(error, ROMX_E_INDEX, 0, reader->info.payload.size,
            "RIDX header fields are invalid");
    }
    entry_count = read_le32(header + 0x08U);
    if (entry_count == UINT32_C(0)) {
        return romx_error_set(error, ROMX_E_INDEX, 0,
            reader->info.payload.size + 0x08U, "RIDX must contain at least one entry");
    }
    available = reader->info.immutable_size - reader->info.payload.size;
    if ((uint64_t)entry_count >
        (available - ROMX_RIDX_HEADER_SIZE) / ROMX_RIDX_ENTRY_SIZE) {
        return romx_error_set(error, ROMX_E_RANGE, 0,
            reader->info.payload.size + 0x08U, "RIDX entry array exceeds immutable content");
    }
    index_size = ROMX_RIDX_HEADER_SIZE + (uint64_t)entry_count * ROMX_RIDX_ENTRY_SIZE;
    if (index_size > (uint64_t)SIZE_MAX ||
        (uint64_t)entry_count > (uint64_t)SIZE_MAX / sizeof(*reader->entries)) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            reader->info.payload.size, "RIDX is too large for this process");
    }
    index_bytes = (uint8_t *)malloc((size_t)index_size);
    reader->entries = (romx_entry_info_t *)calloc((size_t)entry_count,
        sizeof(*reader->entries));
    ranges = (romx_range_t *)calloc((size_t)entry_count, sizeof(*ranges));
    if (index_bytes == NULL || reader->entries == NULL || ranges == NULL) {
        result = romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate RIDX structures");
        goto done;
    }
    result = romx_read_exact(reader, reader->info.payload.size,
        index_bytes, index_size, error);
    if (result != ROMX_OK) goto done;
    expected_crc = read_le32(index_bytes + 0x14U);
    memset(index_bytes + 0x14U, 0, 4U);
    actual_crc = romx_crc32_begin();
    actual_crc = romx_crc32_update(actual_crc, index_bytes, (size_t)index_size);
    actual_crc = romx_crc32_finish(actual_crc);
    if (actual_crc != expected_crc) {
        result = romx_error_set(error, ROMX_E_INDEX, 0,
            reader->info.payload.size + 0x14U, "RIDX CRC32 mismatch");
        goto done;
    }

    for (position = 0U; position < entry_count; ++position) {
        const uint8_t *stored = index_bytes + ROMX_RIDX_HEADER_SIZE +
            (size_t)position * ROMX_RIDX_ENTRY_SIZE;
        romx_entry_info_t *entry = &reader->entries[position];
        uint32_t other;
        entry->struct_size = (uint32_t)sizeof(*entry);
        entry->index = position;
        entry->flags = read_le32(stored + 0x00U);
        entry->format_id = read_le16(stored + 0x04U);
        entry->path_size = read_le16(stored + 0x06U);
        entry->data_offset = read_le64(stored + 0x08U);
        entry->data_size = read_le64(stored + 0x10U);
        entry->crc32 = read_le32(stored + 0x18U);
        if ((entry->flags & ~ROMX_RIDX_FLAGS_MASK) != UINT32_C(0) ||
            read_le32(stored + 0x1CU) != UINT32_C(0) ||
            entry->path_size == UINT32_C(0) ||
            entry->path_size > ROMX_RIDX_PATH_CAPACITY ||
            !bytes_are_zero(stored + 0x20U + entry->path_size,
                ROMX_RIDX_PATH_CAPACITY - entry->path_size) ||
            !path_is_valid(stored + 0x20U, entry->path_size)) {
            result = romx_error_set(error, ROMX_E_VIRTUAL_PATH, 0,
                reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                (uint64_t)position * ROMX_RIDX_ENTRY_SIZE,
                "RIDX entry flags, reserved fields, or virtual path are invalid");
            goto done;
        }
        memcpy(entry->path, stored + 0x20U, entry->path_size);
        entry->path[entry->path_size] = '\0';
        if ((entry->flags & ROMX_RIDX_HAS_CRC32) == UINT32_C(0) &&
            entry->crc32 != UINT32_C(0)) {
            result = romx_error_set(error, ROMX_E_INDEX, 0,
                reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                (uint64_t)position * ROMX_RIDX_ENTRY_SIZE + 0x18U,
                "RIDX CRC32 must be zero when HAS_CRC32 is clear");
            goto done;
        }
        if (entry->data_offset > reader->info.payload.size ||
            entry->data_size > reader->info.payload.size - entry->data_offset) {
            result = romx_error_set(error, ROMX_E_RANGE, 0,
                reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                (uint64_t)position * ROMX_RIDX_ENTRY_SIZE + 0x08U,
                "RIDX entry exceeds payload");
            goto done;
        }
        if ((entry->flags & ROMX_RIDX_ENTRYPOINT) != UINT32_C(0)) {
            ++entrypoint_count;
            reader->info.entrypoint_index = position;
            if (entry->data_offset != UINT64_C(0) ||
                entry->data_size == UINT64_C(0) || entry->format_id == UINT16_C(0) ||
                entry->format_id == UINT16_C(0xffff)) {
                result = romx_error_set(error, ROMX_E_INDEX, 0,
                    reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                    (uint64_t)position * ROMX_RIDX_ENTRY_SIZE,
                    "RIDX entrypoint must be non-empty, typed, and begin at offset zero");
                goto done;
            }
        } else if (entry->format_id == UINT16_C(0xFFFF)) {
            result = romx_error_set(error, ROMX_E_INDEX, 0,
                reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                (uint64_t)position * ROMX_RIDX_ENTRY_SIZE + 0x04U,
                "RIDX entry contains a prohibited format ID");
            goto done;
        }
        for (other = 0U; other < position; ++other) {
            if (ascii_fold_equal(entry->path, reader->entries[other].path)) {
                result = romx_error_set(error, ROMX_E_VIRTUAL_PATH, 0,
                    reader->info.payload.size + ROMX_RIDX_HEADER_SIZE +
                    (uint64_t)position * ROMX_RIDX_ENTRY_SIZE + 0x20U,
                    "RIDX virtual paths collide after ASCII case folding");
                goto done;
            }
        }
        if (entry->data_size != UINT64_C(0)) {
            ranges[range_count].start = entry->data_offset;
            ranges[range_count].end = entry->data_offset + entry->data_size;
            ++range_count;
        }
    }
    if (entrypoint_count != UINT32_C(1)) {
        result = romx_error_set(error, ROMX_E_INDEX, 0,
            reader->info.payload.size, "RIDX must contain exactly one entrypoint");
        goto done;
    }
    if (entry_count == UINT32_C(1) &&
        (reader->entries[0].data_offset != UINT64_C(0) ||
         reader->entries[0].data_size != reader->info.payload.size)) {
        result = romx_error_set(error, ROMX_E_INDEX, 0,
            reader->info.payload.size, "single-file payload must be exact and contiguous");
        goto done;
    }

    qsort(ranges, range_count, sizeof(*ranges), range_compare);
    content_end = UINT64_C(0);
    for (position = 0U; position < range_count; ++position) {
        if (ranges[position].start < content_end) {
            result = romx_error_set(error, ROMX_E_OVERLAP, 0,
                ranges[position].start, "RIDX payload ranges overlap");
            goto done;
        }
        if (ranges[position].start > content_end) {
            result = region_is_zero(reader, content_end,
                ranges[position].start - content_end, error);
            if (result != ROMX_OK) goto done;
        }
        content_end = ranges[position].end;
    }
    if (content_end < reader->info.payload.size) {
        result = region_is_zero(reader, content_end,
            reader->info.payload.size - content_end, error);
        if (result != ROMX_OK) goto done;
    }

    reader->info.entry_count = entry_count;
    reader->info.payload_index.size = index_size;
    content_end = reader->info.payload.size + index_size;
    if (reader->info.metadata.size != UINT64_C(0)) {
        reader->info.metadata.offset = content_end;
    }
    if (reader->info.metadata.size > reader->info.immutable_size - content_end) {
        result = romx_error_set(error, ROMX_E_RANGE, 0, content_end,
            "metadata exceeds immutable content");
        goto done;
    }
    content_end += reader->info.metadata.size;
    if (reader->info.cover.size != UINT64_C(0)) {
        reader->info.cover.offset = content_end;
    }
    if (reader->info.cover.size > reader->info.immutable_size - content_end) {
        result = romx_error_set(error, ROMX_E_RANGE, 0, content_end,
            "cover exceeds immutable content");
        goto done;
    }
    content_end += reader->info.cover.size;
    if (reader->info.mutable_region.size != UINT64_C(0)) {
        uint64_t aligned;
        if (content_end > UINT64_MAX - UINT64_C(4095)) {
            result = romx_error_set(error, ROMX_E_RANGE, 0, content_end,
                "immutable alignment overflows");
            goto done;
        }
        aligned = (content_end + UINT64_C(4095)) & ~UINT64_C(4095);
        if (aligned != reader->info.mutable_region.offset) {
            result = romx_error_set(error, ROMX_E_RANGE, 0, content_end,
                "mutable region does not follow aligned immutable content");
            goto done;
        }
        reader->info.immutable_padding.offset = content_end;
        reader->info.immutable_padding.size = aligned - content_end;
        result = region_is_zero(reader, content_end, aligned - content_end, error);
        if (result != ROMX_OK) goto done;
    } else if (content_end != reader->info.footer.offset) {
        result = romx_error_set(error, ROMX_E_RANGE, 0, content_end,
            "unexpected bytes exist between cover and footer");
        goto done;
    }
    result = ROMX_OK;

done:
    free(index_bytes);
    free(ranges);
    if (result != ROMX_OK) {
        free(reader->entries);
        reader->entries = NULL;
        reader->info.entry_count = UINT32_C(0);
    }
    return result;
}

romx_result_t romx_reader_get_entry_count(const romx_reader_t *reader,
    uint32_t *count, romx_error_t *error)
{
    romx_error_clear(error);
    if (reader == NULL || count == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and count must not be null");
    }
    *count = reader->info.entry_count;
    return ROMX_OK;
}

romx_result_t romx_reader_get_entry(const romx_reader_t *reader,
    uint32_t index, romx_entry_info_t *entry, romx_error_t *error)
{
    uint32_t supplied_size;
    romx_error_clear(error);
    if (reader == NULL || entry == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and entry must not be null");
    }
    supplied_size = entry->struct_size;
    if (supplied_size < sizeof(*entry) || index >= reader->info.entry_count) {
        return romx_error_set(error,
            supplied_size < sizeof(*entry) ? ROMX_E_INVALID_ARGUMENT : ROMX_E_RANGE,
            0, ROMX_OFFSET_UNKNOWN, "entry structure is too small or index is out of range");
    }
    memcpy(entry, &reader->entries[index], sizeof(*entry));
    entry->struct_size = supplied_size;
    return ROMX_OK;
}

romx_result_t romx_reader_get_entrypoint(const romx_reader_t *reader,
    romx_entry_info_t *entry, romx_error_t *error)
{
    if (reader == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader must not be null");
    }
    return romx_reader_get_entry(reader, reader->info.entrypoint_index,
        entry, error);
}

romx_result_t romx_reader_find_entry(const romx_reader_t *reader,
    const char *virtual_path, romx_entry_info_t *entry, romx_error_t *error)
{
    uint32_t index;
    if (reader == NULL || virtual_path == NULL || entry == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader, virtual path, and entry must not be null");
    }
    for (index = 0U; index < reader->info.entry_count; ++index) {
        if (strcmp(reader->entries[index].path, virtual_path) == 0) {
            return romx_reader_get_entry(reader, index, entry, error);
        }
    }
    return romx_error_set(error, ROMX_E_ENTRY_NOT_FOUND, 0,
        ROMX_OFFSET_UNKNOWN, "virtual path is not present in RIDX");
}

romx_result_t romx_reader_read_entry(const romx_reader_t *reader,
    uint32_t index, uint64_t entry_offset, void *buffer,
    uint64_t buffer_size, uint64_t *bytes_read, romx_error_t *error)
{
    const romx_entry_info_t *entry;
    uint64_t count;
    romx_result_t result;
    romx_error_clear(error);
    if (reader == NULL || bytes_read == NULL ||
        (buffer == NULL && buffer_size != UINT64_C(0)) ||
        buffer_size > (uint64_t)SIZE_MAX || index >= reader->info.entry_count) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "invalid RIDX entry read arguments");
    }
    *bytes_read = UINT64_C(0);
    entry = &reader->entries[index];
    if (entry_offset > entry->data_size) {
        return romx_error_set(error, ROMX_E_RANGE, 0, entry_offset,
            "entry-relative offset exceeds entry size");
    }
    count = entry->data_size - entry_offset;
    if (count > buffer_size) count = buffer_size;
    if (count == UINT64_C(0)) return ROMX_OK;
    result = reader->io.read_at(reader->io.user_data,
        entry->data_offset + entry_offset, buffer, count, bytes_read, error);
    if (result != ROMX_OK) return result;
    if (*bytes_read != count) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            entry->data_offset + entry_offset + *bytes_read,
            "RIDX entry read was truncated");
    }
    return ROMX_OK;
}
