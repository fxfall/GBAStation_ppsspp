#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    romx_writer_path_entry_t entry = ROMX_WRITER_PATH_ENTRY_INIT;
    romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
    romx_writer_report_t report = ROMX_WRITER_REPORT_INIT;
    romx_error_t error;
    romx_result_t result;

    if (argc < 6 || argc > 8) {
        fprintf(stderr,
            "usage: %s OUTPUT.romx SOURCE VIRTUAL_PATH PLATFORM_ID FORMAT_ID "
            "[METADATA.json] [COVER.png]\n", argv[0]);
        return 2;
    }
    entry.flags = ROMX_RIDX_ENTRYPOINT | ROMX_RIDX_HAS_CRC32;
    entry.virtual_path = argv[3];
    entry.source_path = argv[2];
    entry.format_id = (uint16_t)strtoul(argv[5], NULL, 0);
    options.flags = ROMX_WRITER_IMMUTABLE_SHA256 |
        ROMX_WRITER_PROBE_PAYLOAD;
    options.platform_id = (uint16_t)strtoul(argv[4], NULL, 0);
    options.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;
    result = romx_writer_write_path_entries(argv[1], &entry, 1U,
        argc >= 7 ? argv[6] : NULL, argc >= 8 ? argv[7] : NULL,
        &options, &report, &error);
    if (result != ROMX_OK) {
        fprintf(stderr, "%s: %s\n", romx_result_string(result), error.message);
        return 1;
    }
    printf("wrote %llu bytes with %u RIDX entry\n",
        (unsigned long long)report.file_size,
        (unsigned int)report.entry_count);
    return 0;
}
