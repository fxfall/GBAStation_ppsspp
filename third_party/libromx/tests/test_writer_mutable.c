#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

typedef struct memory_source {
    const uint8_t *bytes;
    uint64_t size;
} memory_source_t;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
        return 0; \
    } \
} while (0)

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

static romx_io_t memory_io(memory_source_t *source)
{
    romx_io_t io = ROMX_IO_INIT;
    io.user_data = source;
    io.get_size = memory_get_size;
    io.read_at = memory_read_at;
    return io;
}

static uint64_t file_size(const char *path)
{
    FILE *stream = fopen(path, "rb");
#if defined(_WIN32)
    __int64 end;
    if (stream == NULL || _fseeki64(stream, 0, SEEK_END) != 0 ||
        (end = _ftelli64(stream)) < 0) {
#else
    off_t end;
    if (stream == NULL || fseeko(stream, 0, SEEK_END) != 0 ||
        (end = ftello(stream)) < 0) {
#endif
        if (stream != NULL) fclose(stream);
        return UINT64_MAX;
    }
    fclose(stream);
    return (uint64_t)end;
}

static int write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    if (size != 0U && fwrite(bytes, 1U, size, stream) != size) {
        fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int test_multi_entry_and_mutable(const char *directory)
{
    static const uint8_t cue[] =
        "FILE \"track01.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"track02.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n";
    static const uint8_t track1[] = { 1U, 2U, 3U, 4U, 5U };
    static const uint8_t track2[] = { 9U, 8U, 7U };
    static const uint8_t metadata[] =
        "{\"schema_version\":\"0.2.0\",\"name\":\"Writer test\"}";
    static const uint8_t save1[] = { 'S', 'A', 'V', 'E' };
    static const uint8_t save2[] = { 'N', 'E', 'W' };
    memory_source_t sources[3] = {
        { track1, sizeof(track1) }, { cue, sizeof(cue) - 1U },
        { track2, sizeof(track2) }
    };
    romx_io_t ios[3];
    romx_writer_io_entry_t entries[3];
    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    romx_writer_report_t writer_report = ROMX_WRITER_REPORT_INIT;
    romx_validation_report_t validation = ROMX_VALIDATION_REPORT_INIT;
    romx_reader_t *reader = NULL;
    romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
    romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
    romx_mutable_write_options_t mutable_options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
    romx_error_t error;
    romx_io_t save_io;
    memory_source_t save_source;
    char output[1024];
    uint64_t original_size;
    uint32_t count;
    uint8_t first[4];
    uint64_t bytes_read;
    unsigned int index;
    char save_path[1024];
    char rtc_path[1024];

    (void)snprintf(output, sizeof(output), "%s/native-writer.romx", directory);
    (void)unlink(output);
    for (index = 0U; index < 3U; ++index) {
        ios[index] = memory_io(&sources[index]);
        entries[index] = (romx_writer_io_entry_t)ROMX_WRITER_IO_ENTRY_INIT;
        entries[index].source = &ios[index];
        entries[index].flags = ROMX_RIDX_HAS_CRC32;
        entries[index].format_id = ROMX_FORMAT_BIN;
    }
    entries[0].virtual_path = "disc/track01.bin";
    entries[1].virtual_path = "disc/game.cue";
    entries[1].flags |= ROMX_RIDX_ENTRYPOINT;
    entries[1].format_id = ROMX_FORMAT_CUE;
    entries[2].virtual_path = "disc/track02.bin";
    options.flags = ROMX_WRITER_REPLACE_EXISTING |
        ROMX_WRITER_IMMUTABLE_SHA256;
    options.platform_id = ROMX_PLATFORM_PLAYSTATION;
    options.launch_format_id = ROMX_LAUNCH_CUE;
    options.mutable_capacity = UINT64_C(12288);
    options.mutable_entry_capacity = UINT32_C(8);
    CHECK(romx_writer_write_io_entries(output, entries, 3U,
        metadata, sizeof(metadata) - 1U, NULL, &options,
        &writer_report, &error) == ROMX_OK);
    CHECK(writer_report.entry_count == 3U);
    CHECK(writer_report.mutable_capacity == UINT64_C(12288));
    original_size = file_size(output);
    CHECK(original_size == writer_report.file_size);

    CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_entrypoint(reader, &entry, &error) == ROMX_OK);
    CHECK(strcmp(entry.path, "disc/game.cue") == 0);
    CHECK(entry.data_offset == UINT64_C(0));
    CHECK(romx_reader_read_entry(reader, entry.index, 0U, first,
        sizeof(first), &bytes_read, &error) == ROMX_OK);
    CHECK(bytes_read == sizeof(first) && memcmp(first, "FILE", 4U) == 0);
    CHECK(romx_reader_validate(reader,
        ROMX_VALIDATE_IMMUTABLE_SHA256 | ROMX_VALIDATE_ENTRY_CRC32,
        &validation, &error) == ROMX_OK);
    CHECK(validation.immutable_sha256 == ROMX_STATUS_VALID);
    CHECK(validation.entry_crc32 == ROMX_STATUS_VALID);
    romx_reader_close(reader);
    reader = NULL;

    save_source.bytes = save1;
    save_source.size = sizeof(save1);
    save_io = memory_io(&save_source);
    mutable_options.data_capacity = UINT64_C(64);
    mutable_options.modified_unix_seconds = UINT64_C(100);
    CHECK(romx_mutable_write_io_path(output, ROMX_MUTABLE_NAMESPACE_SAVE,
        "slot/save.srm", &save_io, &mutable_options, &object, &error) == ROMX_OK);
    CHECK(object.generation == UINT64_C(1));
    CHECK(object.data_capacity == UINT64_C(64));
    CHECK(file_size(output) == original_size);

    save_source.bytes = save2;
    save_source.size = sizeof(save2);
    object = (romx_mutable_object_info_t)ROMX_MUTABLE_OBJECT_INFO_INIT;
    CHECK(romx_mutable_write_io_path(output, ROMX_MUTABLE_NAMESPACE_SAVE,
        "SLOT/SAVE.SRM", &save_io, &mutable_options, &object, &error) == ROMX_OK);
    CHECK(object.generation == UINT64_C(2));
    CHECK(file_size(output) == original_size);

    CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_mutable_object_count(reader, &count, &error) == ROMX_OK);
    CHECK(count == UINT32_C(1));
    validation = (romx_validation_report_t)ROMX_VALIDATION_REPORT_INIT;
    CHECK(romx_reader_validate(reader, ROMX_VALIDATE_IMMUTABLE_SHA256,
        &validation, &error) == ROMX_OK);
    CHECK(validation.immutable_sha256 == ROMX_STATUS_VALID);
    romx_reader_close(reader);
    reader = NULL;

    CHECK(romx_mutable_delete_path(output, ROMX_MUTABLE_NAMESPACE_SAVE,
        "slot/save.srm", &error) == ROMX_OK);
    CHECK(file_size(output) == original_size);
    CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_mutable_object_count(reader, &count, &error) == ROMX_OK);
    CHECK(count == UINT32_C(0));
    romx_reader_close(reader);

    {
        static const uint8_t savedata[] = { 1U, 3U, 3U, 7U };
        static const uint8_t rtcdata[] = { 2U, 4U, 6U, 8U, 10U };
        romx_mutable_bundle_path_entry_t bundle_entries[2];
        romx_mutable_bundle_path_entry_t collision_entries[3];
        romx_mutable_bundle_t *bundle = NULL;
        romx_mutable_bundle_entry_info_t bundle_entry =
            ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
        romx_mutable_stats_t stats = ROMX_MUTABLE_STATS_INIT;
        romx_mutable_stats_t restored_stats = ROMX_MUTABLE_STATS_INIT;
        uint8_t restored[8];
        uint64_t required = UINT64_C(0);
        uint8_t stats_json[512];

        (void)snprintf(save_path, sizeof(save_path), "%s/game.srm", directory);
        (void)snprintf(rtc_path, sizeof(rtc_path), "%s/game.rtc", directory);
        CHECK(write_bytes(save_path, savedata, sizeof(savedata)));
        CHECK(write_bytes(rtc_path, rtcdata, sizeof(rtcdata)));
        bundle_entries[0] =
            (romx_mutable_bundle_path_entry_t)ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
        bundle_entries[0].relative_path = "game.srm";
        bundle_entries[0].source_path = save_path;
        bundle_entries[1] =
            (romx_mutable_bundle_path_entry_t)ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
        bundle_entries[1].relative_path = "clock/game.rtc";
        bundle_entries[1].source_path = rtc_path;
        mutable_options.data_capacity = UINT64_C(512);
        CHECK(romx_mutable_bundle_write_path_entries(output,
            ROMX_MUTABLE_NAMESPACE_SAVE, "libretro", bundle_entries, 2U,
            NULL, &mutable_options, &object, &error) == ROMX_OK);
        CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
        CHECK(romx_mutable_bundle_open(reader, ROMX_MUTABLE_NAMESPACE_SAVE,
            "libretro", NULL, &bundle, &error) == ROMX_OK);
        CHECK(romx_mutable_bundle_get_entry_count(bundle, &count,
            &error) == ROMX_OK && count == UINT32_C(2));
        CHECK(romx_mutable_bundle_get_entry(bundle, 0U, &bundle_entry,
            &error) == ROMX_OK);
        CHECK(strcmp(bundle_entry.path, "clock/game.rtc") == 0);
        CHECK(romx_mutable_bundle_read_entry(bundle, 0U, UINT64_C(0),
            restored, sizeof(restored), &bytes_read, &error) == ROMX_OK);
        CHECK(bytes_read == sizeof(rtcdata) &&
            memcmp(restored, rtcdata, sizeof(rtcdata)) == 0);
        romx_mutable_bundle_close(bundle);
        romx_reader_close(reader);
        reader = NULL;

        stats.flags = ROMX_MUTABLE_STATS_HAS_PLAY_TIME |
            ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT |
            ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED |
            ROMX_MUTABLE_STATS_HAS_LAST_PLAYED |
            ROMX_MUTABLE_STATS_HAS_FAVORITE |
            ROMX_MUTABLE_STATS_HAS_COMPLETION_PERCENT |
            ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS |
            ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED;
        stats.play_time_seconds = UINT64_C(12345);
        stats.launch_count = UINT64_C(9);
        stats.first_played_unix_seconds = UINT64_C(1000);
        stats.last_played_unix_seconds = UINT64_C(2000);
        stats.favorite = UINT32_C(1);
        stats.completion_percent = UINT32_C(75);
        stats.achievements_unlocked = UINT64_C(12);
        stats.achievements_total = UINT64_C(20);
        stats.achievements_hardcore_unlocked = UINT64_C(4);
        CHECK(romx_mutable_stats_serialize_json(&stats, stats_json,
            sizeof(stats_json), &required, &error) == ROMX_OK);
        CHECK(required > UINT64_C(0));
        CHECK(romx_mutable_stats_parse_json(stats_json, required,
            &restored_stats, &error) == ROMX_OK);
        CHECK(restored_stats.play_time_seconds == stats.play_time_seconds);
        restored_stats = (romx_mutable_stats_t)ROMX_MUTABLE_STATS_INIT;
        CHECK(romx_mutable_stats_parse_json(
            "{\"schema\":\"romx.stats\",\"version\":1,\"unknown\":0}",
            sizeof("{\"schema\":\"romx.stats\",\"version\":1,\"unknown\":0}") -
                1U, &restored_stats, &error) ==
            ROMX_E_MUTABLE_STATS);
        restored_stats = (romx_mutable_stats_t)ROMX_MUTABLE_STATS_INIT;
        CHECK(romx_mutable_stats_parse_json(
            "{\"schema\":\"romx.stats\",\"schema\":\"romx.stats\",\"version\":1}",
            sizeof("{\"schema\":\"romx.stats\",\"schema\":\"romx.stats\",\"version\":1}") -
                1U, &restored_stats, &error) ==
            ROMX_E_MUTABLE_STATS);
        mutable_options.data_capacity = UINT64_C(512);
        CHECK(romx_mutable_stats_write_path(output, "default", &stats,
            &mutable_options, &object, &error) == ROMX_OK);
        CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
        restored_stats = (romx_mutable_stats_t)ROMX_MUTABLE_STATS_INIT;
        CHECK(romx_mutable_stats_read(reader, "default", &restored_stats,
            &error) == ROMX_OK);
        CHECK(restored_stats.achievements_hardcore_unlocked == UINT64_C(4));
        romx_reader_close(reader);
        reader = NULL;
        collision_entries[0] = bundle_entries[0];
        collision_entries[0].relative_path = "A/game.srm";
        collision_entries[1] = bundle_entries[1];
        collision_entries[1].relative_path = "B/game.rtc";
        collision_entries[2] = bundle_entries[0];
        collision_entries[2].relative_path = "a/GAME.SRM";
        CHECK(romx_mutable_bundle_write_path_entries(output,
            ROMX_MUTABLE_NAMESPACE_CHEAT, "collision", collision_entries, 3U,
            NULL, &mutable_options, &object, &error) ==
            ROMX_E_MUTABLE_BUNDLE);
        (void)unlink(save_path);
        (void)unlink(rtc_path);
    }
    (void)unlink(output);
    return 1;
}

static int test_optional_payload_probe(const char *directory)
{
    uint8_t nds[0x400];
    memory_source_t source;
    romx_io_t io;
    romx_writer_io_entry_t entry = ROMX_WRITER_IO_ENTRY_INIT;
    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    romx_reader_t *reader = NULL;
    romx_metadata_t *metadata = NULL;
    romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
    romx_info_t info = ROMX_INFO_INIT;
    romx_error_t error;
    char output[1024];
    char name[64];
    uint64_t required;

    memset(nds, 0, sizeof(nds));
    memcpy(nds, "PROBE NDS", 9U);
    memcpy(nds + 12U, "ABCD", 4U);
    nds[0x68U] = 0x00U;
    nds[0x69U] = 0x01U;
    nds[0x320U + 2U] = 0x1fU;
    memset(nds + 0x120U, 0x11, 512U);
    source.bytes = nds;
    source.size = sizeof(nds);
    io = memory_io(&source);
    entry.flags = ROMX_RIDX_ENTRYPOINT | ROMX_RIDX_HAS_CRC32;
    entry.virtual_path = "game.nds";
    entry.source = &io;
    entry.format_id = ROMX_FORMAT_NDS;
    options.flags = ROMX_WRITER_REPLACE_EXISTING | ROMX_WRITER_PROBE_PAYLOAD;
    options.platform_id = ROMX_PLATFORM_NINTENDO_DS;
    options.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;
    (void)snprintf(output, sizeof(output), "%s/probed-writer.romx", directory);
    (void)unlink(output);
    CHECK(romx_writer_write_io_entries(output, &entry, 1U, NULL, 0U,
        NULL, &options, NULL, &error) == ROMX_OK);
    CHECK(romx_reader_open_path(output, NULL, &reader, &error) == ROMX_OK);
    CHECK(romx_reader_get_info(reader, &info, &error) == ROMX_OK);
    CHECK(info.immutable_hash_algorithm == ROMX_IMMUTABLE_HASH_NONE);
    CHECK(info.metadata.size > UINT64_C(0));
    CHECK(info.cover.size > UINT64_C(0));
    CHECK(romx_metadata_open(reader, &metadata, &error) == ROMX_OK);
    CHECK(romx_metadata_get_string(metadata, "name", name, sizeof(name),
        &required, &error) == ROMX_OK);
    CHECK(strcmp(name, "PROBE NDS") == 0);
    romx_metadata_close(metadata);
    CHECK(romx_reader_validate(reader, ROMX_VALIDATE_COVER,
        &report, &error) == ROMX_OK);
    CHECK(report.cover == ROMX_STATUS_VALID);
    romx_reader_close(reader);
    (void)unlink(output);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    if (!test_multi_entry_and_mutable(argv[1]) ||
        !test_optional_payload_probe(argv[1])) return 1;
    puts("ROMX 0.2.0 writer, mutable bundle/STATS, commit, and probe tests passed");
    return 0;
}
