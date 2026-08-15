#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    romx_probe_t *probe = NULL;
    romx_probe_info_t info = ROMX_PROBE_INFO_INIT;
    romx_error_t error;
    unsigned long format;
    romx_result_t result;
    if (argc != 3) {
        fprintf(stderr, "usage: %s FORMAT_ID ROM_PATH\n", argv[0]);
        return 2;
    }
    format = strtoul(argv[1], NULL, 0);
    if (format > 65535UL) return 2;
    result = romx_probe_open_path(argv[2], (uint16_t)format, &probe, &error);
    if (result != ROMX_OK) {
        fprintf(stderr, "%s: %s\n", romx_result_string(result),
            error.message);
        return result == ROMX_E_UNSUPPORTED ? 3 : 1;
    }
    result = romx_probe_get_info(probe, &info, &error);
    if (result != ROMX_OK) {
        romx_probe_close(probe);
        return 1;
    }
    printf("platform=%s format=%s flags=0x%08x name=%s serial=%s cover=%llu\n",
        romx_platform_name(info.platform_id),
        romx_file_format_name(info.format_id), (unsigned int)info.flags,
        info.name, info.serial, (unsigned long long)info.cover_size);
    romx_probe_close(probe);
    return 0;
}
