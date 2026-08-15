#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory_source {
    uint8_t *bytes;
    uint64_t size;
} memory_source_t;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
        return 0; \
    } \
} while (0)

static void make_path(char *output, size_t capacity,
    const char *directory, const char *name)
{
    (void)snprintf(output, capacity, "%s/%s", directory, name);
}

static uint32_t test_crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    for (index = 0U; index < size; ++index) {
        unsigned int bit;
        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^
                (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static romx_result_t memory_get_size(void *user_data, uint64_t *size,
    romx_error_t *error)
{
    memory_source_t *source = (memory_source_t *)user_data;
    (void)error;
    *size = source->size;
    return ROMX_OK;
}

static romx_result_t memory_read_at(void *user_data, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    memory_source_t *source = (memory_source_t *)user_data;
    uint64_t count;
    (void)error;
    if (offset > source->size) return ROMX_E_RANGE;
    count = source->size - offset;
    if (count > size) count = size;
    if (count != UINT64_C(0)) {
        memcpy(buffer, source->bytes + (size_t)offset, (size_t)count);
    }
    *bytes_read = count;
    return ROMX_OK;
}

static int load_file(const char *path, memory_source_t *source)
{
    FILE *stream = fopen(path, "rb");
    long length;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) return 0;
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        fclose(stream);
        return 0;
    }
    source->bytes = (uint8_t *)malloc((size_t)length);
    source->size = (uint64_t)length;
    if (source->bytes == NULL ||
        fread(source->bytes, 1U, (size_t)length, stream) != (size_t)length) {
        fclose(stream);
        free(source->bytes);
        source->bytes = NULL;
        return 0;
    }
    fclose(stream);
    return 1;
}

static int test_minimal_single(const char *directory)
{
    char path[1024];
    romx_reader_t *reader = NULL;
    romx_vfs_file_t *file = NULL;
    romx_info_t info = ROMX_INFO_INIT;
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    romx_error_t error;
    uint32_t count = 0U;
    uint8_t magic[4];
    uint64_t bytes_read = UINT64_C(0);

    make_path(path, sizeof(path), directory, "minimal-single.romx");
    CHECK(romx_reader_open_path(path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_info(reader, &info, &error) == ROMX_OK);
    CHECK(info.version == ROMX_FORMAT_VERSION);
    CHECK(strcmp(romx_platform_name(info.platform_id), "NES") == 0);
    CHECK(strcmp(romx_launch_format_name(info.launch_format_id),
        "RAW_SINGLE_FILE") == 0);
    CHECK(info.payload.offset == UINT64_C(0));
    CHECK(info.entry_count == UINT32_C(1));
    CHECK(info.payload_index.offset == info.payload.size);
    CHECK(romx_reader_get_entry_count(reader, &count, &error) == ROMX_OK);
    CHECK(count == UINT32_C(1));
    CHECK(romx_reader_get_entrypoint(reader, &entry, &error) == ROMX_OK);
    CHECK(strcmp(entry.path, "game.nes") == 0);
    CHECK(strcmp(romx_file_format_name(entry.format_id), "NES") == 0);
    CHECK(entry.data_offset == UINT64_C(0));
    CHECK(entry.data_size == info.payload.size);
    CHECK(romx_vfs_file_open_entrypoint(reader, &file, &error) == ROMX_OK);
    CHECK(romx_vfs_file_read(file, magic, sizeof(magic),
        &bytes_read, &error) == ROMX_OK);
    CHECK(bytes_read == sizeof(magic));
    CHECK(memcmp(magic, "NES\x1a", sizeof(magic)) == 0);
    romx_vfs_file_close(file);
    romx_reader_close(reader);
    return 1;
}

static int test_multi_file_vfs(const char *directory)
{
    char path[1024];
    romx_reader_t *reader = NULL;
    romx_vfs_file_t *file = NULL;
    romx_payload_file_t *payload_file = NULL;
    romx_payload_mapping_t *mapping = NULL;
    romx_io_t payload_io = ROMX_IO_INIT;
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_error_t error;
    uint8_t cue_prefix[4];
    uint64_t size = UINT64_C(0);
    uint64_t position = UINT64_C(0);
    uint64_t bytes_read = UINT64_C(0);

    make_path(path, sizeof(path), directory, "multi-cue.romx");
    CHECK(romx_reader_open_path(path, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_find_entry(reader, "disc/track01.bin",
        &entry, &error) == ROMX_OK);
    CHECK(entry.data_offset == UINT64_C(135));
    CHECK(entry.data_size == UINT64_C(2560));
    CHECK((entry.flags & ROMX_RIDX_HAS_CRC32) != UINT32_C(0));
    CHECK(romx_vfs_file_open_entrypoint(reader, &file, &error) == ROMX_OK);
    CHECK(romx_vfs_file_get_size(file, &size, &error) == ROMX_OK);
    CHECK(size == UINT64_C(135));
    CHECK(romx_vfs_file_read(file, cue_prefix, sizeof(cue_prefix),
        &bytes_read, &error) == ROMX_OK);
    CHECK(bytes_read == sizeof(cue_prefix));
    CHECK(memcmp(cue_prefix, "FILE", sizeof(cue_prefix)) == 0);
    CHECK(romx_vfs_file_seek(file, -4, ROMX_PAYLOAD_SEEK_END,
        &position, &error) == ROMX_OK);
    CHECK(position == size - UINT64_C(4));
    romx_vfs_file_close(file);
    CHECK(romx_reader_get_payload_io(reader, &payload_io, &error) == ROMX_OK);
    CHECK(payload_io.get_size(payload_io.user_data, &size, &error) == ROMX_OK);
    CHECK(size == UINT64_C(135));
    CHECK(payload_io.read_at(payload_io.user_data, 0U, cue_prefix,
        sizeof(cue_prefix), &bytes_read, &error) == ROMX_OK);
    CHECK(bytes_read == sizeof(cue_prefix));
    CHECK(memcmp(cue_prefix, "FILE", sizeof(cue_prefix)) == 0);
    CHECK(romx_reader_map_payload(reader, &mapping, &error) == ROMX_OK);
    CHECK(romx_payload_mapping_size(mapping) == UINT64_C(135));
    CHECK(memcmp(romx_payload_mapping_data(mapping), "FILE", 4U) == 0);
    romx_payload_mapping_close(mapping);
    CHECK(romx_reader_validate(reader, ROMX_VALIDATE_ENTRY_CRC32,
        &report, &error) == ROMX_OK);
    CHECK(report.entry_crc32 == ROMX_STATUS_VALID);
    romx_reader_close(reader);
    CHECK(romx_payload_file_open_path(path, NULL, NULL,
        &payload_file, &error) == ROMX_OK);
    CHECK(romx_payload_file_get_size(payload_file, &size, &error) == ROMX_OK);
    CHECK(size == UINT64_C(135));
    CHECK(romx_payload_file_read(payload_file, cue_prefix,
        sizeof(cue_prefix), &bytes_read, &error) == ROMX_OK);
    CHECK(memcmp(cue_prefix, "FILE", sizeof(cue_prefix)) == 0);
    romx_payload_file_close(payload_file);
    return 1;
}

static int test_complete_and_mutable_hash_boundary(const char *directory)
{
    char path[1024];
    memory_source_t source = { NULL, UINT64_C(0) };
    romx_io_t io = ROMX_IO_INIT;
    romx_reader_t *reader = NULL;
    romx_metadata_t *metadata = NULL;
    romx_vfs_file_t *file = NULL;
    romx_mutable_file_t *mutable_file = NULL;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_info_t info = ROMX_INFO_INIT;
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_error_t error;
    char name[64];
    uint8_t saved[4];
    uint32_t object_count = UINT32_C(0);
    romx_mutable_status_t mutable_status = ROMX_MUTABLE_ABSENT;
    uint64_t required = UINT64_C(0);

    make_path(path, sizeof(path), directory, "single-complete.romx");
    CHECK(load_file(path, &source));
    io.user_data = &source;
    io.get_size = memory_get_size;
    io.read_at = memory_read_at;
    CHECK(romx_reader_open_io(&io, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_info(reader, &info, &error) == ROMX_OK);
    CHECK(info.metadata.size == UINT64_C(68));
    CHECK(info.cover.size == UINT64_C(70));
    CHECK(info.mutable_region.size == UINT64_C(12288));
    CHECK(info.immutable_size == info.mutable_region.offset);
    CHECK(romx_metadata_open(reader, &metadata, &error) == ROMX_OK);
    CHECK(romx_metadata_get_string(metadata, "name", name, sizeof(name),
        &required, &error) == ROMX_OK);
    CHECK(strcmp(name, "Reference NES") == 0);
    romx_metadata_close(metadata);
    CHECK(romx_reader_validate(reader,
        ROMX_VALIDATE_IMMUTABLE_SHA256 | ROMX_VALIDATE_COVER |
            ROMX_VALIDATE_ENTRY_CRC32,
        &report, &error) == ROMX_OK);
    CHECK(report.immutable_sha256 == ROMX_STATUS_VALID);
    CHECK(report.cover == ROMX_STATUS_VALID);
    CHECK(report.entry_crc32 == ROMX_STATUS_VALID);
    romx_reader_close(reader);

    {
        static const uint8_t value[4] = { 'S', 'A', 'V', 'E' };
        static const char key[] = "slot.sav";
        uint8_t *entry = source.bytes + (size_t)info.mutable_region.offset + 4096U;
        uint8_t *data = source.bytes + (size_t)info.mutable_region.offset + 8192U;
        uint32_t crc;
        memset(entry, 0, 512U);
        memcpy(data, value, sizeof(value));
        memcpy(entry, "MENT", 4U);
        write_le16(entry + 0x04U, UINT16_C(1));
        write_le16(entry + 0x06U, ROMX_MUTABLE_NAMESPACE_SAVE);
        write_le32(entry + 0x0CU, UINT32_C(8));
        write_le64(entry + 0x10U, UINT64_C(8192));
        write_le64(entry + 0x18U, UINT64_C(4096));
        write_le64(entry + 0x20U, sizeof(value));
        write_le64(entry + 0x28U, UINT64_C(1));
        write_le32(entry + 0x38U, test_crc32(data, sizeof(value)));
        memcpy(entry + 0x40U, key, sizeof(key) - 1U);
        crc = test_crc32(entry, 512U);
        write_le32(entry + 0x3CU, crc);
    }
    CHECK(romx_reader_open_io(&io, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_mutable_status(reader, &mutable_status,
        &error) == ROMX_OK);
    CHECK(mutable_status == ROMX_MUTABLE_VALID);
    CHECK(romx_reader_get_mutable_object_count(reader, &object_count,
        &error) == ROMX_OK);
    CHECK(object_count == UINT32_C(1));
    CHECK(romx_reader_find_mutable_object(reader,
        ROMX_MUTABLE_NAMESPACE_SAVE, "slot.sav", &object, &error) == ROMX_OK);
    CHECK(object.data_size == UINT64_C(4));
    CHECK(object.generation == UINT64_C(1));
    CHECK(romx_mutable_file_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
        "slot.sav", &mutable_file, &error) == ROMX_OK);
    CHECK(romx_mutable_file_read(mutable_file, saved, sizeof(saved),
        &required, &error) == ROMX_OK);
    CHECK(required == sizeof(saved));
    CHECK(memcmp(saved, "SAVE", sizeof(saved)) == 0);
    romx_mutable_file_close(mutable_file);
    romx_reader_close(reader);

    source.bytes[(size_t)info.mutable_region.offset + 8192U] ^= UINT8_C(0x01);
    object = (romx_mutable_object_info_t)ROMX_MUTABLE_OBJECT_INFO_INIT;
    CHECK(romx_reader_open_io(&io, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_find_mutable_object(reader,
        ROMX_MUTABLE_NAMESPACE_SAVE, "slot.sav", &object,
        &error) == ROMX_E_MUTABLE_DATA_CRC);
    CHECK(romx_vfs_file_open_entrypoint(reader, &file, &error) == ROMX_OK);
    CHECK(romx_vfs_file_read(file, saved, sizeof(saved),
        &required, &error) == ROMX_OK);
    CHECK(memcmp(saved, "NES\x1a", sizeof(saved)) == 0);
    romx_vfs_file_close(file);
    romx_reader_close(reader);
    source.bytes[(size_t)info.mutable_region.offset + 8192U] ^= UINT8_C(0x01);

    source.bytes[(size_t)info.mutable_region.offset] ^= UINT8_C(0x5a);
    report = (romx_validation_report_t)ROMX_VALIDATION_REPORT_INIT;
    CHECK(romx_reader_open_io(&io, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_mutable_status(reader, &mutable_status,
        &error) == ROMX_OK);
    CHECK(mutable_status == ROMX_MUTABLE_INVALID);
    CHECK(romx_reader_validate(reader, ROMX_VALIDATE_IMMUTABLE_SHA256,
        &report, &error) == ROMX_OK);
    CHECK(report.immutable_sha256 == ROMX_STATUS_VALID);
    romx_reader_close(reader);

    source.bytes[0] ^= UINT8_C(0x01);
    report = (romx_validation_report_t)ROMX_VALIDATION_REPORT_INIT;
    CHECK(romx_reader_open_io(&io, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_validate(reader, ROMX_VALIDATE_IMMUTABLE_SHA256,
        &report, &error) == ROMX_E_IMMUTABLE_HASH);
    romx_reader_close(reader);
    free(source.bytes);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIRECTORY\n", argv[0]);
        return 2;
    }
    if (!test_minimal_single(argv[1]) ||
        !test_multi_file_vfs(argv[1]) ||
        !test_complete_and_mutable_hash_boundary(argv[1])) {
        return 1;
    }
    puts("ROMX 0.2.0 reader and VFS tests passed");
    return 0;
}
