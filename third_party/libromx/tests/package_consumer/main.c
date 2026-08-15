#include <romx/romx.h>

#include <string.h>

int main(int argc, char **argv)
{
    romx_reader_t *reader = NULL;
    romx_error_t error;

    if (strcmp(romx_result_string(ROMX_OK), "success") != 0) {
        return 1;
    }
    if (argc == 2) {
        if (romx_reader_open_path(argv[1], NULL, &reader, &error) != ROMX_OK) {
            return 2;
        }
        romx_reader_close(reader);
    }
    return 0;
}
