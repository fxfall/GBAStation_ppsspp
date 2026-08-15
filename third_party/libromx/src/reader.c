#include "romx_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static romx_result_t romx_validate_options(
    const romx_reader_options_t *options,
    romx_error_t *error)
{
    if (options == NULL) {
        return ROMX_OK;
    }
    if (options->struct_size < sizeof(*options)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader options structure is too small");
    }
    if (options->reserved != UINT32_C(0)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader options reserved field must be zero");
    }
    if (options->io_chunk_size != UINT32_C(0) && options->io_chunk_size < UINT32_C(1024)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "I/O chunk size must be zero or at least 1024 bytes");
    }
    return ROMX_OK;
}

romx_result_t romx_reader_create(
    const romx_io_t *io,
    const romx_reader_options_t *options,
    void (*close_io)(void *user_data),
    romx_reader_t **out_reader,
    romx_error_t *error)
{
    uint8_t footer[ROMX_FOOTER_SIZE];
    uint64_t file_size = UINT64_C(0);
    uint64_t bytes_read = UINT64_C(0);
    romx_reader_t *reader;
    romx_result_t result;

    romx_error_clear(error);
    if (out_reader != NULL) {
        *out_reader = NULL;
    }
    if (io == NULL || out_reader == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "io and out_reader must not be null");
    }
    if (io->struct_size < sizeof(*io) || io->get_size == NULL || io->read_at == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "io structure is incomplete");
    }
    result = romx_validate_options(options, error);
    if (result != ROMX_OK) {
        return result;
    }

    result = io->get_size(io->user_data, &file_size, error);
    if (result != ROMX_OK) {
        if (error != NULL && error->code == ROMX_OK) {
            romx_error_set(error, result, 0, ROMX_OFFSET_UNKNOWN,
                "failed to obtain input size");
        }
        return result;
    }
    if (file_size < ROMX_FOOTER_SIZE) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0, file_size,
            "file is shorter than the ROMX 0.2.0 footer");
    }

    result = io->read_at(io->user_data, file_size - ROMX_FOOTER_SIZE,
        footer, ROMX_FOOTER_SIZE, &bytes_read, error);
    if (result != ROMX_OK) {
        if (error != NULL && error->code == ROMX_OK) {
            romx_error_set(error, result, 0,
                file_size - ROMX_FOOTER_SIZE, "failed to read ROMX footer");
        }
        return result;
    }
    if (bytes_read != ROMX_FOOTER_SIZE) {
        return romx_error_set(error, ROMX_E_TRUNCATED, 0,
            file_size - ROMX_FOOTER_SIZE + bytes_read,
            "ROMX footer read was truncated");
    }

    reader = (romx_reader_t *)calloc(1U, sizeof(*reader));
    if (reader == NULL) {
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate ROMX reader");
    }
    result = romx_parse_footer(footer, file_size, &reader->info, error);
    if (result != ROMX_OK) {
        free(reader);
        return result;
    }
    reader->io = *io;
    reader->max_metadata_size = options != NULL && options->max_metadata_size != UINT64_C(0)
        ? options->max_metadata_size : ROMX_DEFAULT_MAX_METADATA_SIZE;
    reader->max_cover_size = options != NULL && options->max_cover_size != UINT64_C(0)
        ? options->max_cover_size : ROMX_DEFAULT_MAX_COVER_SIZE;
    reader->max_cover_dimension = options != NULL && options->max_cover_dimension != UINT32_C(0)
        ? options->max_cover_dimension : ROMX_DEFAULT_MAX_COVER_DIMENSION;
    reader->io_chunk_size = options != NULL && options->io_chunk_size != UINT32_C(0)
        ? options->io_chunk_size : ROMX_DEFAULT_IO_CHUNK_SIZE;
    result = romx_parse_ridx(reader, error);
    if (result != ROMX_OK) {
        free(reader->entries);
        free(reader);
        return result;
    }
    result = romx_parse_mutable(reader, error);
    if (result != ROMX_OK) {
        free(reader->mutable_slots);
        free(reader->entries);
        free(reader);
        return result;
    }
    reader->close_io = close_io;
    *out_reader = reader;
    return ROMX_OK;
}

romx_result_t romx_reader_open_io(
    const romx_io_t *io,
    const romx_reader_options_t *options,
    romx_reader_t **out_reader,
    romx_error_t *error)
{
    return romx_reader_create(io, options, NULL, out_reader, error);
}

romx_result_t romx_reader_get_info(
    const romx_reader_t *reader,
    romx_info_t *info,
    romx_error_t *error)
{
    uint32_t supplied_size;

    romx_error_clear(error);
    if (reader == NULL || info == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "reader and info must not be null");
    }
    supplied_size = info->struct_size;
    if (supplied_size < sizeof(*info)) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "info structure is too small");
    }
    memcpy(info, &reader->info, sizeof(*info));
    info->struct_size = supplied_size;
    return ROMX_OK;
}

void romx_reader_close(romx_reader_t *reader)
{
    if (reader != NULL && reader->close_io != NULL) {
        reader->close_io(reader->io.user_data);
    }
    if (reader != NULL) {
        free(reader->mutable_slots);
        free(reader->entries);
    }
    free(reader);
}
