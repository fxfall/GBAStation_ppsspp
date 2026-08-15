#include <romx/romx.hpp>

#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 6 || argc > 8) {
        std::cerr << "usage: " << argv[0]
                  << " OUTPUT.romx SOURCE VIRTUAL_PATH PLATFORM_ID FORMAT_ID"
                     " [METADATA.json] [COVER.png]\n";
        return 2;
    }
    try {
        romx_writer_path_entry_t entry = ROMX_WRITER_PATH_ENTRY_INIT;
        romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
        std::vector<romx_writer_path_entry_t> entries;
        entry.flags = ROMX_RIDX_ENTRYPOINT | ROMX_RIDX_HAS_CRC32;
        entry.virtual_path = argv[3];
        entry.source_path = argv[2];
        entry.format_id = static_cast<uint16_t>(std::strtoul(argv[5], nullptr, 0));
        entries.push_back(entry);
        options.flags = ROMX_WRITER_IMMUTABLE_SHA256 |
            ROMX_WRITER_PROBE_PAYLOAD;
        options.platform_id = static_cast<uint16_t>(
            std::strtoul(argv[4], nullptr, 0));
        options.launch_format_id = ROMX_LAUNCH_RAW_SINGLE_FILE;
        romx_writer_report_t report = romx::write_path_entries(argv[1],
            entries, options, argc >= 7 ? argv[6] : "",
            argc >= 8 ? argv[7] : "");
        romx::reader container(argv[1]);
        container.validate();
        std::cout << "wrote " << report.file_size << " bytes\n";
    } catch (const romx::error &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
