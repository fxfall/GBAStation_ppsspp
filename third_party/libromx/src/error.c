#include "romx_internal.h"

#include <string.h>

void romx_error_clear(romx_error_t *error)
{
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->byte_offset = ROMX_OFFSET_UNKNOWN;
    }
}

romx_result_t romx_error_set(
    romx_error_t *error,
    romx_result_t code,
    int32_t system_code,
    uint64_t byte_offset,
    const char *message)
{
    if (error != NULL) {
        size_t length;

        memset(error, 0, sizeof(*error));
        error->code = code;
        error->system_code = system_code;
        error->byte_offset = byte_offset;
        if (message != NULL) {
            length = strlen(message);
            if (length >= sizeof(error->message)) {
                length = sizeof(error->message) - 1U;
            }
            memcpy(error->message, message, length);
            error->message[length] = '\0';
        }
    }
    return code;
}

const char *romx_result_string(romx_result_t result)
{
    switch (result) {
    case ROMX_OK: return "success";
    case ROMX_E_INVALID_ARGUMENT: return "invalid argument";
    case ROMX_E_OUT_OF_MEMORY: return "out of memory";
    case ROMX_E_IO: return "I/O error";
    case ROMX_E_TRUNCATED: return "truncated input";
    case ROMX_E_INVALID_FOOTER: return "invalid ROMX footer";
    case ROMX_E_INVALID_FLAGS: return "invalid ROMX flags";
    case ROMX_E_RANGE: return "ROMX region is out of range";
    case ROMX_E_OVERLAP: return "ROMX regions overlap";
    case ROMX_E_IMMUTABLE_HASH: return "ROMX immutable SHA-256 mismatch";
    case ROMX_E_METADATA_ABSENT: return "ROMX metadata is absent";
    case ROMX_E_METADATA_TOO_LARGE: return "ROMX metadata exceeds the limit";
    case ROMX_E_METADATA_UTF8: return "ROMX metadata is not valid UTF-8";
    case ROMX_E_METADATA_JSON: return "ROMX metadata is not valid JSON";
    case ROMX_E_METADATA_SCHEMA: return "ROMX metadata does not match schema 0.2.0";
    case ROMX_E_COVER_ABSENT: return "ROMX cover is absent";
    case ROMX_E_COVER_TOO_LARGE: return "ROMX cover exceeds the limit";
    case ROMX_E_COVER_PNG: return "ROMX cover is not a valid PNG";
    case ROMX_E_EXTRACT_HASH: return "extracted bytes failed SHA-256 verification";
    case ROMX_E_BUFFER_TOO_SMALL: return "output buffer is too small";
    case ROMX_E_WRITE: return "output write failed";
    case ROMX_E_ATOMIC_RENAME: return "atomic output replacement failed";
    case ROMX_E_EXISTS: return "output already exists";
    case ROMX_E_UNSUPPORTED: return "operation is not supported for this input";
    case ROMX_E_INDEX: return "invalid ROMX payload index";
    case ROMX_E_VIRTUAL_PATH: return "invalid ROMX virtual path";
    case ROMX_E_ENTRY_NOT_FOUND: return "ROMX virtual entry not found";
    case ROMX_E_ENTRY_CRC: return "ROMX entry CRC32 mismatch";
    case ROMX_E_MUTABLE_ABSENT: return "ROMX mutable region is absent";
    case ROMX_E_MUTABLE_HEADER: return "ROMX mutable header is invalid";
    case ROMX_E_MUTABLE_ENTRY: return "ROMX mutable object is invalid or absent";
    case ROMX_E_MUTABLE_DATA_CRC: return "ROMX mutable object CRC32 mismatch";
    case ROMX_E_MUTABLE_NO_SPACE: return "ROMX mutable region has insufficient space";
    case ROMX_E_MUTABLE_BUNDLE: return "ROMX mutable bundle is invalid";
    case ROMX_E_MUTABLE_STATS: return "ROMX mutable statistics JSON is invalid";
    default: return "unknown libromx result";
    }
}
