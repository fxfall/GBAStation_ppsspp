#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct romx_probe {
    romx_probe_info_t info;
    uint8_t *metadata;
    size_t metadata_size;
    uint8_t *cover;
    size_t cover_size;
    uint32_t cover_width;
    uint32_t cover_height;
};

typedef struct probe_path_input {
    FILE *file;
    uint64_t size;
} probe_path_input_t;

typedef struct probe_memory_input {
    const uint8_t *bytes;
    uint64_t size;
} probe_memory_input_t;

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static romx_result_t io_size(const romx_io_t *io, uint64_t *size,
    romx_error_t *error)
{
    if (io == NULL || io->struct_size < sizeof(*io) ||
        io->get_size == NULL || io->read_at == NULL) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "probe input is incomplete");
    }
    return io->get_size(io->user_data, size, error);
}

static romx_result_t read_exact(const romx_io_t *io, uint64_t offset,
    void *buffer, uint64_t size, romx_error_t *error)
{
    uint64_t total = UINT64_C(0);
    while (total < size) {
        uint64_t count = UINT64_C(0);
        romx_result_t result = io->read_at(io->user_data, offset + total,
            (uint8_t *)buffer + (size_t)total, size - total, &count, error);
        if (result != ROMX_OK) return result;
        if (count == 0U || count > size - total) return ROMX_E_TRUNCATED;
        total += count;
    }
    return ROMX_OK;
}

static romx_result_t probe_memory_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    probe_memory_input_t *input = (probe_memory_input_t *)user;
    (void)error;
    *size = input->size;
    return ROMX_OK;
}

static romx_result_t probe_memory_read(void *user, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    probe_memory_input_t *input = (probe_memory_input_t *)user;
    uint64_t count;
    (void)error;
    if (offset > input->size) return ROMX_E_RANGE;
    count = input->size - offset;
    if (count > size) count = size;
    if (count != UINT64_C(0))
        memcpy(buffer, input->bytes + (size_t)offset, (size_t)count);
    *bytes_read = count;
    return ROMX_OK;
}

static void validate_probed_cover(romx_probe_t *probe)
{
    probe_memory_input_t input;
    romx_io_t io = ROMX_IO_INIT;
    uint8_t hash[32];
    uint32_t width = 0U;
    uint32_t height = 0U;
    if ((probe->info.flags & ROMX_PROBE_HAS_COVER) == 0U) return;
    input.bytes = probe->cover;
    input.size = probe->cover_size;
    io.user_data = &input;
    io.get_size = probe_memory_get_size;
    io.read_at = probe_memory_read;
    if (romx_validate_cover_io(&io, input.size,
        ROMX_DEFAULT_MAX_COVER_SIZE, ROMX_DEFAULT_MAX_COVER_DIMENSION,
        ROMX_DEFAULT_IO_CHUNK_SIZE, hash, &width, &height, NULL) != ROMX_OK) {
        free(probe->cover);
        probe->cover = NULL;
        probe->cover_size = 0U;
        probe->cover_width = 0U;
        probe->cover_height = 0U;
        probe->info.cover_size = UINT64_C(0);
        probe->info.flags &= ~ROMX_PROBE_HAS_COVER;
    } else {
        probe->cover_width = width;
        probe->cover_height = height;
    }
}

static void copy_header_text(char *output, size_t capacity,
    const uint8_t *input, size_t size)
{
    size_t begin = 0U;
    size_t end = size;
    size_t target = 0U;
    while (begin < end && (input[begin] == 0U || input[begin] == (uint8_t)' ' ||
        input[begin] == UINT8_C(0xff))) ++begin;
    while (end > begin && (input[end - 1U] == 0U ||
        input[end - 1U] == (uint8_t)' ' || input[end - 1U] == UINT8_C(0xff))) --end;
    while (begin < end && target + 1U < capacity) {
        uint8_t byte = input[begin++];
        if (byte >= UINT8_C(0x20) && byte <= UINT8_C(0x7e)) {
            output[target++] = (char)byte;
        }
    }
    output[target] = '\0';
}

static void copy_utf8_text(char *output, size_t capacity,
    const uint8_t *input, size_t size)
{
    size_t end = 0U;
    size_t bad_offset = 0U;
    size_t source;
    size_t target = 0U;
    while (end < size && input[end] != UINT8_C(0)) ++end;
    while (end > 0U && input[end - 1U] == (uint8_t)' ') --end;
    if (!romx_utf8_validate(input, end, &bad_offset)) {
        copy_header_text(output, capacity, input, end);
        return;
    }
    for (source = 0U; source < end && target + 1U < capacity; ++source) {
        uint8_t value = input[source];
        if (value < UINT8_C(0x20)) continue;
        if (value < UINT8_C(0x80)) {
            output[target++] = (char)value;
        } else {
            size_t sequence = value < UINT8_C(0xe0) ? 2U :
                (value < UINT8_C(0xf0) ? 3U : 4U);
            if (sequence > end - source || sequence > capacity - 1U - target)
                break;
            memcpy(output + target, input + source, sequence);
            target += sequence;
            source += sequence - 1U;
        }
    }
    output[target] = '\0';
}

static int header_text_score(const uint8_t *input, size_t size)
{
    size_t index;
    int printable = 0;
    for (index = 0U; index < size; ++index) {
        if (input[index] >= UINT8_C(0x20) && input[index] <= UINT8_C(0x7e)) {
            ++printable;
        } else if (input[index] != UINT8_C(0) && input[index] != UINT8_C(0xff)) {
            printable -= 2;
        }
    }
    return printable;
}

static romx_result_t probe_mega_drive(const romx_io_t *io, uint64_t size,
    uint16_t format, romx_probe_t *probe, romx_error_t *error)
{
    uint8_t header[0x100];
    if (format == ROMX_FORMAT_SMD) {
        uint8_t block[16384];
        uint8_t logical[16384];
        size_t index;
        if (size < UINT64_C(0x200)) return ROMX_E_UNSUPPORTED;
        if (read_exact(io, UINT64_C(0x100), header, sizeof(header), error) != ROMX_OK)
            return ROMX_E_UNSUPPORTED;
        /* Some dumps retain .smd while already stored in linear byte order. */
        if (memcmp(header, "SEGA", 4U) != 0) {
            if (size < UINT64_C(512 + 16384) ||
                read_exact(io, UINT64_C(512), block, sizeof(block), error) != ROMX_OK)
                return ROMX_E_UNSUPPORTED;
            for (index = 0U; index < 8192U; ++index) {
                logical[index * 2U] = block[8192U + index];
                logical[index * 2U + 1U] = block[index];
            }
            memcpy(header, logical + 0x100U, sizeof(header));
        }
    } else {
        if (size < UINT64_C(0x200) ||
            read_exact(io, UINT64_C(0x100), header, sizeof(header), error) != ROMX_OK)
            return ROMX_E_UNSUPPORTED;
    }
    if (memcmp(header, "SEGA", 4U) != 0) return ROMX_E_UNSUPPORTED;
    copy_header_text(probe->info.name, sizeof(probe->info.name),
        header + 0x50U, 48U);
    if (probe->info.name[0] == '\0') {
        copy_header_text(probe->info.name, sizeof(probe->info.name),
            header + 0x20U, 48U);
    }
    copy_header_text(probe->info.serial, sizeof(probe->info.serial),
        header + 0x80U, 14U);
    probe->info.platform_id = format == ROMX_FORMAT_X32
        ? ROMX_PLATFORM_MEGA_DRIVE_32X : ROMX_PLATFORM_MEGA_DRIVE;
    return ROMX_OK;
}

static romx_result_t probe_snes(const romx_io_t *io, uint64_t size,
    romx_probe_t *probe, romx_error_t *error)
{
    static const uint64_t bases[] = {
        UINT64_C(0x7fc0), UINT64_C(0xffc0), UINT64_C(0x40ffc0),
        UINT64_C(0x81c0), UINT64_C(0x101c0), UINT64_C(0x4101c0)
    };
    uint8_t best[32] = { 0 };
    int best_score = -1000;
    size_t candidate;
    for (candidate = 0U; candidate < sizeof(bases) / sizeof(bases[0]); ++candidate) {
        uint8_t header[32];
        uint16_t complement;
        uint16_t checksum;
        int score;
        if (bases[candidate] > size || sizeof(header) > size - bases[candidate]) continue;
        if (read_exact(io, bases[candidate], header, sizeof(header), error) != ROMX_OK)
            continue;
        complement = read_le16(header + 0x1cU);
        checksum = read_le16(header + 0x1eU);
        score = header_text_score(header, 21U);
        if ((uint16_t)(complement ^ checksum) == UINT16_C(0xffff)) score += 24;
        if ((header[0x15U] & UINT8_C(0x0f)) <= UINT8_C(5)) score += 4;
        if (score > best_score) {
            best_score = score;
            memcpy(best, header, sizeof(best));
        }
    }
    if (best_score < 8) return ROMX_E_UNSUPPORTED;
    copy_header_text(probe->info.name, sizeof(probe->info.name), best, 21U);
    probe->info.platform_id = ROMX_PLATFORM_SNES;
    return probe->info.name[0] != '\0' ? ROMX_OK : ROMX_E_UNSUPPORTED;
}

static void normalize_n64_header(uint8_t *bytes, size_t size)
{
    size_t index;
    if (size >= 4U && memcmp(bytes, "\x37\x80\x40\x12", 4U) == 0) {
        for (index = 0U; index + 1U < size; index += 2U) {
            uint8_t value = bytes[index];
            bytes[index] = bytes[index + 1U];
            bytes[index + 1U] = value;
        }
    } else if (size >= 4U && memcmp(bytes, "\x40\x12\x37\x80", 4U) == 0) {
        for (index = 0U; index + 3U < size; index += 4U) {
            uint8_t a = bytes[index];
            uint8_t b = bytes[index + 1U];
            bytes[index] = bytes[index + 3U];
            bytes[index + 1U] = bytes[index + 2U];
            bytes[index + 2U] = b;
            bytes[index + 3U] = a;
        }
    }
}

static size_t json_escape(const char *input, char *output, size_t capacity)
{
    size_t size = 0U;
    while (*input != '\0') {
        char value = *input++;
        if (value == '"' || value == '\\') {
            if (output != NULL && size + 2U <= capacity) {
                output[size] = '\\'; output[size + 1U] = value;
            }
            size += 2U;
        } else {
            if (output != NULL && size < capacity) output[size] = value;
            ++size;
        }
    }
    return size;
}

static romx_result_t build_metadata(romx_probe_t *probe,
    romx_error_t *error)
{
    size_t escaped_name = json_escape(probe->info.name, NULL, 0U);
    size_t escaped_serial = json_escape(probe->info.serial, NULL, 0U);
    size_t capacity = escaped_name + escaped_serial + 160U;
    char *json = (char *)malloc(capacity);
    size_t position = 0U;
    int count;
    if (json == NULL) return ROMX_E_OUT_OF_MEMORY;
    count = snprintf(json, capacity, "{\"schema_version\":\"0.2.0\",\"name\":\"");
    if (count < 0) { free(json); return ROMX_E_WRITE; }
    position = (size_t)count;
    position += json_escape(probe->info.name, json + position, capacity - position);
    json[position++] = '"';
    if ((probe->info.flags & ROMX_PROBE_HAS_SERIAL) != 0U) {
        memcpy(json + position, ",\"serial\":\"", 11U); position += 11U;
        position += json_escape(probe->info.serial, json + position, capacity - position);
        json[position++] = '"';
    }
    if ((probe->info.flags & ROMX_PROBE_HAS_COVER) != 0U) {
        count = snprintf(json + position, capacity - position,
            ",\"cover\":{\"mime_type\":\"image/png\",\"width\":%u,\"height\":%u}",
            (unsigned int)probe->cover_width, (unsigned int)probe->cover_height);
        if (count < 0) { free(json); return ROMX_E_WRITE; }
        position += (size_t)count;
    }
    json[position++] = '}';
    if (romx_validate_metadata_bytes((const uint8_t *)json, position,
        error) != ROMX_OK) { free(json); return ROMX_E_METADATA_SCHEMA; }
    probe->metadata = (uint8_t *)json;
    probe->metadata_size = position;
    return ROMX_OK;
}

static uint32_t adler32(const uint8_t *bytes, size_t size)
{
    uint32_t a = 1U, b = 0U;
    size_t index;
    for (index = 0U; index < size; ++index) {
        a = (a + bytes[index]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

static void png_chunk(uint8_t *output, const char type[4],
    const uint8_t *data, uint32_t size)
{
    uint32_t crc = romx_crc32_begin();
    write_be32(output, size);
    memcpy(output + 4U, type, 4U);
    if (size != 0U) memcpy(output + 8U, data, size);
    crc = romx_crc32_update(crc, output + 4U, 4U + size);
    crc = romx_crc32_finish(crc);
    write_be32(output + 8U + size, crc);
}

static romx_result_t encode_rgba_png(const uint8_t *rgba,
    uint32_t width, uint32_t height, uint8_t **output, size_t *output_size)
{
    size_t row = 1U + (size_t)width * 4U;
    size_t raw_size = row * height;
    size_t blocks = (raw_size + 65534U) / 65535U;
    size_t zsize = 2U + raw_size + blocks * 5U + 4U;
    size_t total = 8U + 25U + 12U + zsize + 12U;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    uint8_t *png = (uint8_t *)malloc(total);
    uint8_t *zlib;
    size_t source = 0U, target = 2U, y;
    if (raw == NULL || png == NULL) { free(raw); free(png); return ROMX_E_OUT_OF_MEMORY; }
    for (y = 0U; y < height; ++y) {
        raw[y * row] = 0U;
        memcpy(raw + y * row + 1U, rgba + y * (size_t)width * 4U,
            (size_t)width * 4U);
    }
    memcpy(png, "\x89PNG\r\n\x1a\n", 8U);
    {
        uint8_t ihdr[13] = { 0 };
        write_be32(ihdr, width); write_be32(ihdr + 4U, height);
        ihdr[8] = 8U; ihdr[9] = 6U;
        png_chunk(png + 8U, "IHDR", ihdr, 13U);
    }
    zlib = png + 33U + 8U;
    zlib[0] = UINT8_C(0x78); zlib[1] = UINT8_C(0x01);
    while (source < raw_size) {
        uint16_t count = (uint16_t)((raw_size - source) > 65535U
            ? 65535U : (raw_size - source));
        int final = source + count == raw_size;
        zlib[target++] = (uint8_t)(final ? 1U : 0U);
        zlib[target++] = (uint8_t)count; zlib[target++] = (uint8_t)(count >> 8);
        zlib[target++] = (uint8_t)~count; zlib[target++] = (uint8_t)(~count >> 8);
        memcpy(zlib + target, raw + source, count);
        target += count; source += count;
    }
    write_be32(zlib + target, adler32(raw, raw_size)); target += 4U;
    png_chunk(png + 33U, "IDAT", zlib, (uint32_t)target);
    png_chunk(png + 33U + 12U + target, "IEND", NULL, 0U);
    free(raw); *output = png; *output_size = total; return ROMX_OK;
}

static romx_result_t probe_nds_icon(const romx_io_t *io, uint64_t size,
    romx_probe_t *probe, romx_error_t *error)
{
    uint8_t offset_bytes[4];
    uint8_t banner[0x240];
    uint8_t rgba[32U * 32U * 4U];
    uint32_t banner_offset;
    unsigned int tile_y, tile_x, pixel_y, pixel_x;
    if (size < UINT64_C(0x6c) || read_exact(io, 0x68U,
        offset_bytes, sizeof(offset_bytes), error) != ROMX_OK) return ROMX_OK;
    banner_offset = read_le32(offset_bytes);
    if (banner_offset == 0U || banner_offset > size ||
        sizeof(banner) > size - banner_offset ||
        read_exact(io, banner_offset, banner, sizeof(banner), error) != ROMX_OK)
        return ROMX_OK;
    memset(rgba, 0, sizeof(rgba));
    for (tile_y = 0U; tile_y < 4U; ++tile_y) for (tile_x = 0U; tile_x < 4U; ++tile_x)
        for (pixel_y = 0U; pixel_y < 8U; ++pixel_y) for (pixel_x = 0U; pixel_x < 8U; ++pixel_x) {
            unsigned int tile = tile_y * 4U + tile_x;
            unsigned int pixel = pixel_y * 8U + pixel_x;
            uint8_t packed = banner[0x20U + tile * 32U + pixel / 2U];
            unsigned int palette_index = (pixel & 1U) != 0U ? packed >> 4 : packed & 0x0fU;
            uint16_t color = read_le16(banner + 0x220U + palette_index * 2U);
            unsigned int x = tile_x * 8U + pixel_x;
            unsigned int y = tile_y * 8U + pixel_y;
            uint8_t *out = rgba + (y * 32U + x) * 4U;
            out[0] = (uint8_t)(((color & 31U) * 255U) / 31U);
            out[1] = (uint8_t)((((color >> 5) & 31U) * 255U) / 31U);
            out[2] = (uint8_t)((((color >> 10) & 31U) * 255U) / 31U);
            out[3] = (uint8_t)(palette_index == 0U ? 0U : 255U);
        }
    if (encode_rgba_png(rgba, 32U, 32U, &probe->cover,
        &probe->cover_size) == ROMX_OK) {
        probe->cover_width = 32U; probe->cover_height = 32U;
        probe->info.flags |= ROMX_PROBE_HAS_COVER;
        probe->info.cover_size = probe->cover_size;
    }
    return ROMX_OK;
}

static int sfo_value(const uint8_t *sfo, size_t size, const char *wanted,
    char *output, size_t capacity)
{
    uint32_t key_offset, data_offset, count, index;
    if (size < 20U || memcmp(sfo, "\0PSF", 4U) != 0) return 0;
    key_offset = read_le32(sfo + 8U); data_offset = read_le32(sfo + 12U);
    count = read_le32(sfo + 16U);
    if (count > (size - 20U) / 16U) return 0;
    for (index = 0U; index < count; ++index) {
        const uint8_t *entry = sfo + 20U + index * 16U;
        uint16_t key = read_le16(entry);
        uint32_t length = read_le32(entry + 4U);
        uint32_t data = read_le32(entry + 12U);
        if (key_offset + key >= size || data_offset + data >= size ||
            length > size - (data_offset + data)) continue;
        if (strncmp((const char *)sfo + key_offset + key, wanted,
            size - (key_offset + key)) == 0) {
            copy_utf8_text(output, capacity, sfo + data_offset + data, length);
            return output[0] != '\0';
        }
    }
    return 0;
}

static void set_sfo_fields(romx_probe_t *probe, const uint8_t *sfo, size_t size)
{
    if (sfo_value(sfo, size, "TITLE", probe->info.name,
        sizeof(probe->info.name))) probe->info.flags |= ROMX_PROBE_HAS_NAME;
    if (sfo_value(sfo, size, "DISC_ID", probe->info.serial,
        sizeof(probe->info.serial)) || sfo_value(sfo, size, "TITLE_ID",
        probe->info.serial, sizeof(probe->info.serial)))
        probe->info.flags |= ROMX_PROBE_HAS_SERIAL;
}

static romx_result_t probe_pbp(const romx_io_t *io, uint64_t size,
    romx_probe_t *probe, romx_error_t *error)
{
    uint8_t header[40];
    uint32_t param_start, icon_start, icon_end;
    uint8_t *param;
    char category[16] = { 0 };
    if (size < sizeof(header) || read_exact(io, 0U, header, sizeof(header), error) != ROMX_OK ||
        memcmp(header, "\0PBP", 4U) != 0) return ROMX_E_UNSUPPORTED;
    param_start = read_le32(header + 8U); icon_start = read_le32(header + 12U);
    icon_end = read_le32(header + 16U);
    if (param_start > icon_start || icon_start > icon_end || icon_end > size) return ROMX_E_UNSUPPORTED;
    param = (uint8_t *)malloc(icon_start - param_start);
    if (param == NULL) return ROMX_E_OUT_OF_MEMORY;
    if (read_exact(io, param_start, param, icon_start - param_start, error) == ROMX_OK) {
        set_sfo_fields(probe, param, icon_start - param_start);
        if (sfo_value(param, icon_start - param_start, "CATEGORY",
            category, sizeof(category)) && strcmp(category, "ME") == 0) {
            probe->info.platform_id = ROMX_PLATFORM_PLAYSTATION;
        }
    }
    free(param);
    if (icon_end > icon_start && icon_end - icon_start <= ROMX_DEFAULT_MAX_COVER_SIZE) {
        uint8_t dimensions[24];
        probe->cover = (uint8_t *)malloc(icon_end - icon_start);
        if (probe->cover == NULL) return ROMX_E_OUT_OF_MEMORY;
        if (read_exact(io, icon_start, probe->cover, icon_end - icon_start, error) != ROMX_OK) return ROMX_E_IO;
        probe->cover_size = icon_end - icon_start;
        if (probe->cover_size >= sizeof(dimensions)) {
            memcpy(dimensions, probe->cover, sizeof(dimensions));
            if (memcmp(dimensions, "\x89PNG\r\n\x1a\n", 8U) == 0) {
                probe->cover_width = read_be32(dimensions + 16U);
                probe->cover_height = read_be32(dimensions + 20U);
                probe->info.flags |= ROMX_PROBE_HAS_COVER;
                probe->info.cover_size = probe->cover_size;
            }
        }
    }
    return ROMX_OK;
}

static int iso_name_equal(const uint8_t *name, size_t size, const char *wanted)
{
    size_t index;
    const char *semicolon;
    if (size == 1U && (name[0] == 0U || name[0] == 1U)) return 0;
    semicolon = memchr(name, ';', size);
    if (semicolon != NULL) size = (size_t)(semicolon - (const char *)name);
    if (strlen(wanted) != size) return 0;
    for (index = 0U; index < size; ++index) {
        unsigned char a = name[index], b = (unsigned char)wanted[index];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 32U);
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 32U);
        if (a != b) return 0;
    }
    return 1;
}

static int iso_find(const romx_io_t *io, uint32_t directory_extent,
    uint32_t directory_size, const char *wanted, uint32_t *extent,
    uint32_t *size, int *is_directory)
{
    uint8_t *bytes;
    size_t position = 0U;
    if (directory_size == 0U || directory_size > 4U * 1024U * 1024U) return 0;
    bytes = (uint8_t *)malloc(directory_size);
    if (bytes == NULL || read_exact(io, (uint64_t)directory_extent * 2048U,
        bytes, directory_size, NULL) != ROMX_OK) { free(bytes); return 0; }
    while (position < directory_size) {
        uint8_t length = bytes[position];
        if (length == 0U) { position = (position + 2048U) & ~(size_t)2047U; continue; }
        if (length < 34U || length > directory_size - position) break;
        if (33U + bytes[position + 32U] <= length &&
            iso_name_equal(bytes + position + 33U, bytes[position + 32U], wanted)) {
            *extent = read_le32(bytes + position + 2U);
            *size = read_le32(bytes + position + 10U);
            *is_directory = (bytes[position + 25U] & 2U) != 0U;
            free(bytes); return 1;
        }
        position += length;
    }
    free(bytes); return 0;
}

static romx_result_t probe_psp_iso(const romx_io_t *io, uint64_t size,
    romx_probe_t *probe, romx_error_t *error)
{
    uint8_t pvd[2048];
    uint32_t root_extent, root_size, game_extent, game_size;
    uint32_t param_extent, param_size, icon_extent, icon_size;
    int is_directory;
    uint8_t *param;
    if (size < UINT64_C(17) * 2048U || read_exact(io, UINT64_C(16) * 2048U,
        pvd, sizeof(pvd), error) != ROMX_OK || pvd[0] != 1U ||
        memcmp(pvd + 1U, "CD001", 5U) != 0) return ROMX_E_UNSUPPORTED;
    root_extent = read_le32(pvd + 158U); root_size = read_le32(pvd + 166U);
    if (!iso_find(io, root_extent, root_size, "PSP_GAME", &game_extent,
        &game_size, &is_directory) || !is_directory) return ROMX_E_UNSUPPORTED;
    if (iso_find(io, game_extent, game_size, "PARAM.SFO", &param_extent,
        &param_size, &is_directory) && !is_directory && param_size <= 1024U * 1024U) {
        param = (uint8_t *)malloc(param_size);
        if (param == NULL) return ROMX_E_OUT_OF_MEMORY;
        if (read_exact(io, (uint64_t)param_extent * 2048U, param,
            param_size, error) == ROMX_OK) set_sfo_fields(probe, param, param_size);
        free(param);
    }
    if (iso_find(io, game_extent, game_size, "ICON0.PNG", &icon_extent,
        &icon_size, &is_directory) && !is_directory &&
        icon_size >= 24U && icon_size <= ROMX_DEFAULT_MAX_COVER_SIZE) {
        probe->cover = (uint8_t *)malloc(icon_size);
        if (probe->cover == NULL) return ROMX_E_OUT_OF_MEMORY;
        if (read_exact(io, (uint64_t)icon_extent * 2048U, probe->cover,
            icon_size, error) != ROMX_OK) return ROMX_E_IO;
        if (memcmp(probe->cover, "\x89PNG\r\n\x1a\n", 8U) == 0) {
            probe->cover_size = icon_size;
            probe->cover_width = read_be32(probe->cover + 16U);
            probe->cover_height = read_be32(probe->cover + 20U);
            probe->info.flags |= ROMX_PROBE_HAS_COVER;
            probe->info.cover_size = icon_size;
        }
    }
    return ROMX_OK;
}

static romx_result_t probe_headers(const romx_io_t *io, uint64_t size,
    uint16_t format, romx_probe_t *probe, romx_error_t *error)
{
    uint8_t bytes[192];
    size_t read_size = size < sizeof(bytes) ? (size_t)size : sizeof(bytes);
    romx_result_t special_result;
    probe->info.format_id = format;
    if (format == ROMX_FORMAT_MD || format == ROMX_FORMAT_GEN ||
        format == ROMX_FORMAT_SMD || format == ROMX_FORMAT_X32) {
        special_result = probe_mega_drive(io, size, format, probe, error);
        if (special_result != ROMX_OK) return special_result;
        goto finish;
    }
    if (format == ROMX_FORMAT_SFC || format == ROMX_FORMAT_SMC) {
        special_result = probe_snes(io, size, probe, error);
        if (special_result != ROMX_OK) return special_result;
        goto finish;
    }
    if (read_size != 0U && read_exact(io, 0U, bytes, read_size, error) != ROMX_OK)
        return ROMX_E_IO;
    switch (format) {
    case ROMX_FORMAT_GB:
    case ROMX_FORMAT_GBC:
        if (size < 0x144U) return ROMX_E_UNSUPPORTED;
        if (read_exact(io, 0x134U, bytes, 16U, error) != ROMX_OK) return ROMX_E_IO;
        copy_header_text(probe->info.name, sizeof(probe->info.name), bytes,
            (bytes[15] == 0x80U || bytes[15] == 0xc0U) ? 11U : 16U);
        probe->info.platform_id = bytes[15] == 0xc0U
            ? ROMX_PLATFORM_GAME_BOY_COLOR :
            (format == ROMX_FORMAT_GBC ? ROMX_PLATFORM_GAME_BOY_COLOR : ROMX_PLATFORM_GAME_BOY);
        break;
    case ROMX_FORMAT_GBA:
        if (size < 0xb3U || bytes[0xb2U] != UINT8_C(0x96))
            return ROMX_E_UNSUPPORTED;
        copy_header_text(probe->info.name, sizeof(probe->info.name), bytes + 0xa0U, 12U);
        copy_header_text(probe->info.serial, sizeof(probe->info.serial), bytes + 0xacU, 4U);
        probe->info.platform_id = ROMX_PLATFORM_GAME_BOY_ADVANCE;
        break;
    case ROMX_FORMAT_NDS:
        if (size < 0x70U) return ROMX_E_UNSUPPORTED;
        copy_header_text(probe->info.name, sizeof(probe->info.name), bytes, 12U);
        copy_header_text(probe->info.serial, sizeof(probe->info.serial), bytes + 12U, 4U);
        probe->info.platform_id = ROMX_PLATFORM_NINTENDO_DS;
        (void)probe_nds_icon(io, size, probe, error);
        break;
    case ROMX_FORMAT_N64:
    case ROMX_FORMAT_Z64:
    case ROMX_FORMAT_V64:
        if (size < 0x34U) return ROMX_E_UNSUPPORTED;
        normalize_n64_header(bytes, read_size);
        if (memcmp(bytes, "\x80\x37\x12\x40", 4U) != 0)
            return ROMX_E_UNSUPPORTED;
        copy_header_text(probe->info.name, sizeof(probe->info.name), bytes + 0x20U, 20U);
        probe->info.platform_id = ROMX_PLATFORM_NINTENDO_64;
        break;
    case ROMX_FORMAT_PBP:
        probe->info.platform_id = ROMX_PLATFORM_PSP;
        return probe_pbp(io, size, probe, error);
    case ROMX_FORMAT_ISO:
        probe->info.platform_id = ROMX_PLATFORM_PSP;
        return probe_psp_iso(io, size, probe, error);
    default:
        return ROMX_E_UNSUPPORTED;
    }
finish:
    if (probe->info.name[0] != '\0') probe->info.flags |= ROMX_PROBE_HAS_NAME;
    if (probe->info.serial[0] != '\0') probe->info.flags |= ROMX_PROBE_HAS_SERIAL;
    return ROMX_OK;
}

romx_result_t romx_probe_open_io(const romx_io_t *payload,
    uint16_t format_hint, romx_probe_t **out_probe, romx_error_t *error)
{
    romx_probe_t *probe;
    uint64_t size;
    romx_result_t result;
    romx_error_clear(error);
    if (out_probe != NULL) *out_probe = NULL;
    if (out_probe == NULL || format_hint == ROMX_FORMAT_UNKNOWN) {
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "probe output and format hint are required");
    }
    result = io_size(payload, &size, error);
    if (result != ROMX_OK) return result;
    probe = (romx_probe_t *)calloc(1U, sizeof(*probe));
    if (probe == NULL) return ROMX_E_OUT_OF_MEMORY;
    probe->info = (romx_probe_info_t)ROMX_PROBE_INFO_INIT;
    result = probe_headers(payload, size, format_hint, probe, error);
    if (result != ROMX_OK) { romx_probe_close(probe); return result; }
    validate_probed_cover(probe);
    if ((probe->info.flags & ROMX_PROBE_HAS_NAME) != 0U) {
        result = build_metadata(probe, error);
        if (result != ROMX_OK) { romx_probe_close(probe); return result; }
    }
    *out_probe = probe; return ROMX_OK;
}

static romx_result_t path_get_size(void *user, uint64_t *size,
    romx_error_t *error)
{
    probe_path_input_t *input = (probe_path_input_t *)user;
    (void)error; *size = input->size; return ROMX_OK;
}

static romx_result_t path_read_at(void *user, uint64_t offset,
    void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
    probe_path_input_t *input = (probe_path_input_t *)user;
    size_t count;
#if defined(_WIN32)
    if (_fseeki64(input->file, (__int64)offset, SEEK_SET) != 0)
#else
    if (fseeko(input->file, (off_t)offset, SEEK_SET) != 0)
#endif
        return romx_error_set(error, ROMX_E_IO, 0, offset, "probe seek failed");
    count = fread(buffer, 1U, (size_t)size, input->file);
    *bytes_read = count;
    return ferror(input->file) ? ROMX_E_IO : ROMX_OK;
}

romx_result_t romx_probe_open_path(const char *path, uint16_t format_hint,
    romx_probe_t **out_probe, romx_error_t *error)
{
    probe_path_input_t input;
    romx_io_t io = ROMX_IO_INIT;
    romx_result_t result;
 #if defined(_WIN32)
    __int64 end;
 #else
    off_t end;
 #endif
    if (path == NULL) return ROMX_E_INVALID_ARGUMENT;
    input.file = fopen(path, "rb");
#if defined(_WIN32)
    if (input.file == NULL || _fseeki64(input.file, 0, SEEK_END) != 0 ||
        (end = _ftelli64(input.file)) < 0 || _fseeki64(input.file, 0, SEEK_SET) != 0) {
#else
    if (input.file == NULL || fseeko(input.file, 0, SEEK_END) != 0 ||
        (end = ftello(input.file)) < 0 || fseeko(input.file, 0, SEEK_SET) != 0) {
#endif
        if (input.file != NULL) fclose(input.file);
        return romx_error_set(error, ROMX_E_IO, 0,
            ROMX_OFFSET_UNKNOWN, "failed to open probe path");
    }
    input.size = (uint64_t)end;
    io.user_data = &input; io.get_size = path_get_size; io.read_at = path_read_at;
    result = romx_probe_open_io(&io, format_hint, out_probe, error);
    fclose(input.file); return result;
}

romx_result_t romx_probe_get_info(const romx_probe_t *probe,
    romx_probe_info_t *info, romx_error_t *error)
{
    uint32_t supplied;
    if (probe == NULL || info == NULL || info->struct_size < sizeof(*info))
        return ROMX_E_INVALID_ARGUMENT;
    supplied = info->struct_size; memcpy(info, &probe->info, sizeof(*info));
    info->struct_size = supplied; romx_error_clear(error); return ROMX_OK;
}

static romx_result_t copy_bytes(const uint8_t *bytes, size_t size,
    void *buffer, uint64_t capacity, uint64_t *required, romx_error_t *error)
{
    if (required == NULL) return ROMX_E_INVALID_ARGUMENT;
    *required = size;
    if (bytes == NULL) return ROMX_E_UNSUPPORTED;
    if (buffer == NULL || capacity < size) return ROMX_E_BUFFER_TOO_SMALL;
    memcpy(buffer, bytes, size); romx_error_clear(error); return ROMX_OK;
}

romx_result_t romx_probe_copy_metadata_json(const romx_probe_t *probe,
    void *buffer, uint64_t capacity, uint64_t *required, romx_error_t *error)
{
    if (probe == NULL) return ROMX_E_INVALID_ARGUMENT;
    return copy_bytes(probe->metadata, probe->metadata_size,
        buffer, capacity, required, error);
}

romx_result_t romx_probe_copy_cover_png(const romx_probe_t *probe,
    void *buffer, uint64_t capacity, uint64_t *required, romx_error_t *error)
{
    if (probe == NULL) return ROMX_E_INVALID_ARGUMENT;
    return copy_bytes(probe->cover, probe->cover_size,
        buffer, capacity, required, error);
}

void romx_probe_close(romx_probe_t *probe)
{
    if (probe != NULL) { free(probe->metadata); free(probe->cover); free(probe); }
}
