#ifndef ROMX_ROMX_H
#define ROMX_ROMX_H

#include <stddef.h>
#include <stdint.h>

#include <romx/romx_version.h>

#if defined(_WIN32) && defined(ROMX_SHARED)
#  if defined(ROMX_BUILDING_LIBRARY)
#    define ROMX_API __declspec(dllexport)
#  else
#    define ROMX_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ROMX_API __attribute__((visibility("default")))
#else
#  define ROMX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ROMX_FORMAT_VERSION UINT32_C(2)
#define ROMX_FOOTER_SIZE UINT32_C(128)
#define ROMX_RIDX_HEADER_SIZE UINT32_C(64)
#define ROMX_RIDX_ENTRY_SIZE UINT32_C(512)
#define ROMX_RIDX_PATH_CAPACITY UINT32_C(480)

#define ROMX_IMMUTABLE_HASH_NONE UINT32_C(0)
#define ROMX_IMMUTABLE_HASH_SHA256 UINT32_C(1)

#define ROMX_RIDX_ENTRYPOINT UINT32_C(0x00000001)
#define ROMX_RIDX_HAS_CRC32 UINT32_C(0x00000002)
#define ROMX_RIDX_FLAGS_MASK UINT32_C(0x00000003)

/* ROMX 0.2.0 platform registry. */
#define ROMX_PLATFORM_UNSPECIFIED       UINT16_C(0x0000)
#define ROMX_PLATFORM_GAME_BOY          UINT16_C(0x0001)
#define ROMX_PLATFORM_GAME_BOY_COLOR    UINT16_C(0x0002)
#define ROMX_PLATFORM_GAME_BOY_ADVANCE  UINT16_C(0x0003)
#define ROMX_PLATFORM_NES               UINT16_C(0x0004)
#define ROMX_PLATFORM_SNES              UINT16_C(0x0005)
#define ROMX_PLATFORM_NINTENDO_64       UINT16_C(0x0006)
#define ROMX_PLATFORM_NINTENDO_DS       UINT16_C(0x0007)
#define ROMX_PLATFORM_NINTENDO_3DS      UINT16_C(0x0008)
#define ROMX_PLATFORM_MASTER_SYSTEM     UINT16_C(0x0010)
#define ROMX_PLATFORM_GAME_GEAR         UINT16_C(0x0011)
#define ROMX_PLATFORM_MEGA_DRIVE        UINT16_C(0x0012)
#define ROMX_PLATFORM_MEGA_DRIVE_32X    UINT16_C(0x0013)
#define ROMX_PLATFORM_SEGA_CD           UINT16_C(0x0014)
#define ROMX_PLATFORM_SEGA_SATURN       UINT16_C(0x0015)
#define ROMX_PLATFORM_DREAMCAST         UINT16_C(0x0016)
#define ROMX_PLATFORM_PC_ENGINE         UINT16_C(0x0020)
#define ROMX_PLATFORM_PC_ENGINE_CD      UINT16_C(0x0021)
#define ROMX_PLATFORM_PLAYSTATION       UINT16_C(0x0030)
#define ROMX_PLATFORM_PLAYSTATION_2     UINT16_C(0x0031)
#define ROMX_PLATFORM_PSP               UINT16_C(0x0032)
#define ROMX_PLATFORM_GAMECUBE          UINT16_C(0x0040)
#define ROMX_PLATFORM_WII               UINT16_C(0x0041)
#define ROMX_PLATFORM_ARCADE            UINT16_C(0x0050)
#define ROMX_PLATFORM_SCUMMVM           UINT16_C(0x0060)
#define ROMX_PLATFORM_DOS               UINT16_C(0x0061)
#define ROMX_PLATFORM_AMIGA             UINT16_C(0x0062)

/* ROMX 0.2.0 launch-format registry. */
#define ROMX_LAUNCH_UNSPECIFIED         UINT16_C(0x0000)
#define ROMX_LAUNCH_RAW_SINGLE_FILE     UINT16_C(0x0001)
#define ROMX_LAUNCH_CUE                 UINT16_C(0x0002)
#define ROMX_LAUNCH_GDI                 UINT16_C(0x0003)
#define ROMX_LAUNCH_M3U                 UINT16_C(0x0004)
#define ROMX_LAUNCH_CCD                 UINT16_C(0x0005)
#define ROMX_LAUNCH_MDS                 UINT16_C(0x0006)
#define ROMX_LAUNCH_TOC                 UINT16_C(0x0007)
#define ROMX_LAUNCH_DIRECTORY           UINT16_C(0x0008)
#define ROMX_LAUNCH_ROMSET              UINT16_C(0x0009)
#define ROMX_LAUNCH_SPLIT_FILE_SET      UINT16_C(0x000a)

/* Frequently used RIDX file-format values. Unknown registered/private values
 * remain readable and are returned numerically through romx_entry_info_t. */
#define ROMX_FORMAT_UNKNOWN UINT16_C(0x0000)
#define ROMX_FORMAT_GB      UINT16_C(0x0001)
#define ROMX_FORMAT_GBC     UINT16_C(0x0002)
#define ROMX_FORMAT_GBA     UINT16_C(0x0003)
#define ROMX_FORMAT_NES     UINT16_C(0x0004)
#define ROMX_FORMAT_UNF     UINT16_C(0x0005)
#define ROMX_FORMAT_UNIF    UINT16_C(0x0006)
#define ROMX_FORMAT_FDS     UINT16_C(0x0007)
#define ROMX_FORMAT_SFC     UINT16_C(0x0008)
#define ROMX_FORMAT_SMC     UINT16_C(0x0009)
#define ROMX_FORMAT_NDS     UINT16_C(0x000a)
#define ROMX_FORMAT_N3DS    UINT16_C(0x000b)
#define ROMX_FORMAT_CCI     UINT16_C(0x000c)
#define ROMX_FORMAT_CXI     UINT16_C(0x000d)
#define ROMX_FORMAT_APP     UINT16_C(0x000e)
#define ROMX_FORMAT_ISO     UINT16_C(0x0010)
#define ROMX_FORMAT_CSO     UINT16_C(0x0011)
#define ROMX_FORMAT_ZSO     UINT16_C(0x0012)
#define ROMX_FORMAT_CHD     UINT16_C(0x0013)
#define ROMX_FORMAT_PBP     UINT16_C(0x0014)
#define ROMX_FORMAT_CDI     UINT16_C(0x0015)
#define ROMX_FORMAT_GCM     UINT16_C(0x0016)
#define ROMX_FORMAT_WBFS    UINT16_C(0x0017)
#define ROMX_FORMAT_RVZ     UINT16_C(0x0018)
#define ROMX_FORMAT_WIA     UINT16_C(0x0019)
#define ROMX_FORMAT_WAD     UINT16_C(0x001a)
#define ROMX_FORMAT_CUE     UINT16_C(0x0020)
#define ROMX_FORMAT_GDI     UINT16_C(0x0021)
#define ROMX_FORMAT_M3U     UINT16_C(0x0022)
#define ROMX_FORMAT_CCD     UINT16_C(0x0023)
#define ROMX_FORMAT_MDS     UINT16_C(0x0024)
#define ROMX_FORMAT_TOC     UINT16_C(0x0025)
#define ROMX_FORMAT_BIN     UINT16_C(0x0030)
#define ROMX_FORMAT_WAV     UINT16_C(0x0031)
#define ROMX_FORMAT_FLAC    UINT16_C(0x0032)
#define ROMX_FORMAT_IMG     UINT16_C(0x0033)
#define ROMX_FORMAT_MDF     UINT16_C(0x0034)
#define ROMX_FORMAT_SBI     UINT16_C(0x0040)
#define ROMX_FORMAT_SUB     UINT16_C(0x0041)
#define ROMX_FORMAT_ECM     UINT16_C(0x0042)
#define ROMX_FORMAT_Z64     UINT16_C(0x0050)
#define ROMX_FORMAT_N64     UINT16_C(0x0051)
#define ROMX_FORMAT_V64     UINT16_C(0x0052)
#define ROMX_FORMAT_MD      UINT16_C(0x0060)
#define ROMX_FORMAT_GEN     UINT16_C(0x0061)
#define ROMX_FORMAT_SMD     UINT16_C(0x0062)
#define ROMX_FORMAT_X32     UINT16_C(0x0063)
#define ROMX_FORMAT_SMS     UINT16_C(0x0064)
#define ROMX_FORMAT_GG      UINT16_C(0x0065)
#define ROMX_FORMAT_PCE     UINT16_C(0x0066)
#define ROMX_FORMAT_ELF     UINT16_C(0x0070)
#define ROMX_FORMAT_PRX     UINT16_C(0x0071)
#define ROMX_FORMAT_MSU     UINT16_C(0x0080)
#define ROMX_FORMAT_PCM     UINT16_C(0x0081)
#define ROMX_FORMAT_ROMX_LAUNCH_DESCRIPTOR UINT16_C(0x0090)

#define ROMX_ERROR_MESSAGE_CAPACITY 256
#define ROMX_OFFSET_UNKNOWN UINT64_MAX

typedef int32_t romx_result_t;

#define ROMX_OK                         INT32_C(0)
#define ROMX_E_INVALID_ARGUMENT        INT32_C(-1)
#define ROMX_E_OUT_OF_MEMORY           INT32_C(-2)
#define ROMX_E_IO                      INT32_C(-3)
#define ROMX_E_TRUNCATED               INT32_C(-4)
#define ROMX_E_INVALID_FOOTER          INT32_C(-5)
#define ROMX_E_INVALID_FLAGS           INT32_C(-6)
#define ROMX_E_RANGE                   INT32_C(-7)
#define ROMX_E_OVERLAP                 INT32_C(-8)
#define ROMX_E_IMMUTABLE_HASH          INT32_C(-9)
#define ROMX_E_METADATA_ABSENT         INT32_C(-10)
#define ROMX_E_METADATA_TOO_LARGE      INT32_C(-11)
#define ROMX_E_METADATA_UTF8           INT32_C(-12)
#define ROMX_E_METADATA_JSON           INT32_C(-13)
#define ROMX_E_METADATA_SCHEMA         INT32_C(-14)
#define ROMX_E_COVER_ABSENT            INT32_C(-15)
#define ROMX_E_COVER_TOO_LARGE         INT32_C(-16)
#define ROMX_E_COVER_PNG               INT32_C(-17)
#define ROMX_E_EXTRACT_HASH            INT32_C(-18)
#define ROMX_E_BUFFER_TOO_SMALL        INT32_C(-19)
#define ROMX_E_WRITE                   INT32_C(-20)
#define ROMX_E_ATOMIC_RENAME           INT32_C(-21)
#define ROMX_E_EXISTS                  INT32_C(-22)
#define ROMX_E_UNSUPPORTED             INT32_C(-23)
#define ROMX_E_INDEX                   INT32_C(-24)
#define ROMX_E_VIRTUAL_PATH            INT32_C(-25)
#define ROMX_E_ENTRY_NOT_FOUND         INT32_C(-26)
#define ROMX_E_ENTRY_CRC               INT32_C(-27)
#define ROMX_E_MUTABLE_ABSENT          INT32_C(-28)
#define ROMX_E_MUTABLE_HEADER          INT32_C(-29)
#define ROMX_E_MUTABLE_ENTRY           INT32_C(-30)
#define ROMX_E_MUTABLE_DATA_CRC        INT32_C(-31)
#define ROMX_E_MUTABLE_NO_SPACE        INT32_C(-32)
#define ROMX_E_MUTABLE_BUNDLE          INT32_C(-33)
#define ROMX_E_MUTABLE_STATS           INT32_C(-34)

typedef struct romx_error {
    romx_result_t code;
    int32_t system_code;
    uint64_t byte_offset;
    char message[ROMX_ERROR_MESSAGE_CAPACITY];
} romx_error_t;

typedef struct romx_io {
    uint32_t struct_size;
    void *user_data;
    romx_result_t (*get_size)(
        void *user_data,
        uint64_t *size,
        romx_error_t *error);
    romx_result_t (*read_at)(
        void *user_data,
        uint64_t offset,
        void *buffer,
        uint64_t size,
        uint64_t *bytes_read,
        romx_error_t *error);
} romx_io_t;

#define ROMX_IO_INIT { (uint32_t)sizeof(romx_io_t), NULL, NULL, NULL }

typedef struct romx_reader_options {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t max_metadata_size;
    uint64_t max_cover_size;
    uint32_t max_cover_dimension;
    uint32_t io_chunk_size;
} romx_reader_options_t;

#define ROMX_READER_OPTIONS_INIT \
    { (uint32_t)sizeof(romx_reader_options_t), UINT32_C(0), \
      UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0) }

#define ROMX_DEFAULT_MAX_METADATA_SIZE UINT64_C(1048576)
#define ROMX_DEFAULT_MAX_COVER_SIZE UINT64_C(33554432)
#define ROMX_DEFAULT_MAX_COVER_DIMENSION UINT32_C(8192)
#define ROMX_DEFAULT_IO_CHUNK_SIZE UINT32_C(65536)

typedef struct romx_region_info {
    uint64_t offset;
    uint64_t size;
} romx_region_info_t;

typedef struct romx_info {
    uint32_t struct_size;
    uint32_t version;
    uint64_t file_size;
    romx_region_info_t payload;
    romx_region_info_t metadata;
    romx_region_info_t cover;
    romx_region_info_t payload_index;
    romx_region_info_t immutable_padding;
    romx_region_info_t mutable_region;
    romx_region_info_t footer;
    uint64_t immutable_size;
    uint32_t immutable_hash_algorithm;
    uint32_t footer_crc32;
    uint16_t platform_id;
    uint16_t launch_format_id;
    uint32_t entry_count;
    uint32_t entrypoint_index;
    uint8_t immutable_sha256[32];
} romx_info_t;

#define ROMX_INFO_INIT { \
    (uint32_t)sizeof(romx_info_t), UINT32_C(0), UINT64_C(0), \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    { UINT64_C(0), UINT64_C(0) }, \
    UINT64_C(0), UINT32_C(0), UINT32_C(0), UINT16_C(0), UINT16_C(0), \
    UINT32_C(0), UINT32_C(0), { 0 } \
}

typedef struct romx_reader romx_reader_t;
typedef struct romx_metadata romx_metadata_t;
typedef struct romx_payload_mapping romx_payload_mapping_t;
typedef struct romx_payload_file romx_payload_file_t;
typedef struct romx_vfs_file romx_vfs_file_t;
typedef struct romx_mutable_file romx_mutable_file_t;
typedef struct romx_mutable_bundle romx_mutable_bundle_t;
typedef struct romx_probe romx_probe_t;

typedef struct romx_entry_info {
    uint32_t struct_size;
    uint32_t index;
    uint32_t flags;
    uint16_t format_id;
    uint16_t reserved;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t crc32;
    uint32_t path_size;
    char path[ROMX_RIDX_PATH_CAPACITY + 1U];
} romx_entry_info_t;

#define ROMX_ENTRY_INFO_INIT { \
    (uint32_t)sizeof(romx_entry_info_t), UINT32_C(0), UINT32_C(0), \
    UINT16_C(0), UINT16_C(0), UINT64_C(0), UINT64_C(0), \
    UINT32_C(0), UINT32_C(0), { 0 } \
}

typedef int32_t romx_mutable_status_t;
#define ROMX_MUTABLE_ABSENT   INT32_C(0)
#define ROMX_MUTABLE_VALID    INT32_C(1)
#define ROMX_MUTABLE_DEGRADED INT32_C(2)
#define ROMX_MUTABLE_INVALID  INT32_C(3)

typedef uint16_t romx_mutable_namespace_t;
#define ROMX_MUTABLE_NAMESPACE_SAVE    UINT16_C(1)
#define ROMX_MUTABLE_NAMESPACE_CHEAT   UINT16_C(2)
#define ROMX_MUTABLE_NAMESPACE_STATS   UINT16_C(3)
#define ROMX_MUTABLE_NAMESPACE_PRIVATE UINT16_C(4)

#define ROMX_MUTABLE_KEY_CAPACITY UINT32_C(448)

typedef struct romx_mutable_object_info {
    uint32_t struct_size;
    uint32_t slot_index;
    romx_mutable_namespace_t object_namespace;
    uint16_t reserved;
    uint64_t data_offset;
    uint64_t data_capacity;
    uint64_t data_size;
    uint64_t generation;
    uint64_t modified_unix_seconds;
    uint32_t data_crc32;
    uint32_t key_size;
    char key[ROMX_MUTABLE_KEY_CAPACITY + 1U];
} romx_mutable_object_info_t;

#define ROMX_MUTABLE_OBJECT_INFO_INIT { \
    (uint32_t)sizeof(romx_mutable_object_info_t), UINT32_C(0), \
    UINT16_C(0), UINT16_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
    UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0), { 0 } \
}

typedef int32_t romx_region_t;
#define ROMX_REGION_PAYLOAD   INT32_C(1)
#define ROMX_REGION_METADATA  INT32_C(2)
#define ROMX_REGION_COVER     INT32_C(3)
#define ROMX_REGION_PAYLOAD_INDEX INT32_C(4)
#define ROMX_REGION_MUTABLE      INT32_C(5)
#define ROMX_REGION_IMMUTABLE    INT32_C(6)

/* Independent, read-only cursor over the RIDX entrypoint. */
typedef uint32_t romx_payload_file_flags_t;
#define ROMX_PAYLOAD_FILE_VALIDATE_IMMUTABLE_SHA256 UINT32_C(0x00000001)

typedef struct romx_payload_file_options {
    uint32_t struct_size;
    romx_payload_file_flags_t flags;
    uint32_t reserved;
} romx_payload_file_options_t;

#define ROMX_PAYLOAD_FILE_OPTIONS_INIT { \
    (uint32_t)sizeof(romx_payload_file_options_t), UINT32_C(0), UINT32_C(0) \
}

typedef int32_t romx_payload_seek_position_t;
#define ROMX_PAYLOAD_SEEK_START   INT32_C(0)
#define ROMX_PAYLOAD_SEEK_CURRENT INT32_C(1)
#define ROMX_PAYLOAD_SEEK_END     INT32_C(2)

typedef uint32_t romx_validate_flags_t;
#define ROMX_VALIDATE_PAYLOAD_HASHES UINT32_C(0x00000001) /* derived values; no payload hash is stored */
#define ROMX_VALIDATE_IMMUTABLE_SHA256 UINT32_C(0x00000002)
#define ROMX_VALIDATE_METADATA    UINT32_C(0x00000004)
#define ROMX_VALIDATE_COVER       UINT32_C(0x00000008)
#define ROMX_VALIDATE_ENTRY_CRC32 UINT32_C(0x00000010)
#define ROMX_VALIDATE_ALL         UINT32_C(0x0000001f)

typedef int32_t romx_validation_status_t;
#define ROMX_STATUS_NOT_CHECKED INT32_C(0)
#define ROMX_STATUS_VALID       INT32_C(1)
#define ROMX_STATUS_INVALID     INT32_C(2)
#define ROMX_STATUS_ABSENT      INT32_C(3)

typedef struct romx_validation_report {
    uint32_t struct_size;
    romx_validation_status_t structure;
    romx_validation_status_t payload_hashes; /* derived CRC/digest calculation */
    romx_validation_status_t immutable_sha256;
    romx_validation_status_t metadata;
    romx_validation_status_t cover;
    romx_validation_status_t cover_hashes; /* derived API value; no cover hash is normative */
    romx_result_t metadata_result;
    romx_result_t cover_result;
    uint32_t computed_payload_crc32;
    uint8_t computed_payload_sha256[32];
    uint8_t computed_immutable_sha256[32];
    uint8_t computed_cover_sha256[32]; /* derived only; never metadata */
    uint32_t cover_width;
    uint32_t cover_height;
    romx_validation_status_t entry_crc32;
} romx_validation_report_t;

#define ROMX_VALIDATION_REPORT_INIT { \
    (uint32_t)sizeof(romx_validation_report_t), \
    ROMX_STATUS_NOT_CHECKED, ROMX_STATUS_NOT_CHECKED, \
    ROMX_STATUS_NOT_CHECKED, ROMX_STATUS_NOT_CHECKED, \
    ROMX_STATUS_NOT_CHECKED, ROMX_STATUS_NOT_CHECKED, \
    ROMX_OK, ROMX_OK, UINT32_C(0), { 0 }, { 0 }, { 0 }, \
    UINT32_C(0), UINT32_C(0), ROMX_STATUS_NOT_CHECKED \
}

typedef struct romx_sink {
    uint32_t struct_size;
    void *user_data;
    romx_result_t (*write)(
        void *user_data,
        const void *data,
        uint64_t size,
        romx_error_t *error);
    romx_result_t (*flush)(void *user_data, romx_error_t *error);
} romx_sink_t;

#define ROMX_SINK_INIT { (uint32_t)sizeof(romx_sink_t), NULL, NULL, NULL }

typedef uint32_t romx_extract_flags_t;
#define ROMX_EXTRACT_REPLACE_EXISTING UINT32_C(0x00000001)
#define ROMX_EXTRACT_DURABLE          UINT32_C(0x00000002)

typedef struct romx_extract_options {
    uint32_t struct_size;
    romx_extract_flags_t flags;
} romx_extract_options_t;

#define ROMX_EXTRACT_OPTIONS_INIT \
    { (uint32_t)sizeof(romx_extract_options_t), UINT32_C(0) }

typedef struct romx_cover_info {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint64_t size;
    uint8_t sha256[32]; /* derived API value; not stored in ROMX metadata */
} romx_cover_info_t;

#define ROMX_COVER_INFO_INIT \
    { (uint32_t)sizeof(romx_cover_info_t), UINT32_C(0), UINT32_C(0), \
      UINT64_C(0), { 0 } }

typedef uint32_t romx_writer_flags_t;
#define ROMX_WRITER_IMMUTABLE_SHA256 UINT32_C(0x00000001)
#define ROMX_WRITER_REPLACE_EXISTING UINT32_C(0x00000002)
#define ROMX_WRITER_DURABLE          UINT32_C(0x00000004)
#define ROMX_WRITER_PROBE_PAYLOAD    UINT32_C(0x00000008)

typedef struct romx_writer_io_entry {
    uint32_t struct_size;
    uint32_t flags;
    const char *virtual_path;
    const romx_io_t *source;
    uint16_t format_id;
    uint16_t reserved;
} romx_writer_io_entry_t;

#define ROMX_WRITER_IO_ENTRY_INIT { \
    (uint32_t)sizeof(romx_writer_io_entry_t), UINT32_C(0), NULL, NULL, \
    UINT16_C(0), UINT16_C(0) \
}

typedef struct romx_writer_path_entry {
    uint32_t struct_size;
    uint32_t flags;
    const char *virtual_path;
    const char *source_path;
    uint16_t format_id;
    uint16_t reserved;
} romx_writer_path_entry_t;

#define ROMX_WRITER_PATH_ENTRY_INIT { \
    (uint32_t)sizeof(romx_writer_path_entry_t), UINT32_C(0), NULL, NULL, \
    UINT16_C(0), UINT16_C(0) \
}

typedef struct romx_writer_options {
    uint32_t struct_size;
    romx_writer_flags_t flags;
    uint16_t platform_id;
    uint16_t launch_format_id;
    uint64_t mutable_capacity;
    uint32_t mutable_entry_capacity;
    uint64_t max_metadata_size;
    uint64_t max_cover_size;
    uint32_t max_cover_dimension;
    uint32_t io_chunk_size;
} romx_writer_options_t;

#define ROMX_WRITER_OPTIONS_INIT { \
    (uint32_t)sizeof(romx_writer_options_t), UINT32_C(0), \
    UINT16_C(0), UINT16_C(0), UINT64_C(0), UINT32_C(0), \
    UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0) \
}

typedef struct romx_writer_report {
    uint32_t struct_size;
    uint64_t file_size;
    uint64_t payload_size;
    uint64_t payload_index_size;
    uint64_t metadata_size;
    uint64_t cover_size;
    uint64_t immutable_padding_size;
    uint64_t mutable_capacity;
    uint32_t entry_count;
    uint32_t immutable_hash_algorithm;
    uint8_t immutable_sha256[32];
} romx_writer_report_t;

#define ROMX_WRITER_REPORT_INIT { \
    (uint32_t)sizeof(romx_writer_report_t), \
    UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
    UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0), { 0 } \
}

typedef uint32_t romx_probe_flags_t;
#define ROMX_PROBE_HAS_NAME   UINT32_C(0x00000001)
#define ROMX_PROBE_HAS_SERIAL UINT32_C(0x00000002)
#define ROMX_PROBE_HAS_COVER  UINT32_C(0x00000004)

typedef struct romx_probe_info {
    uint32_t struct_size;
    romx_probe_flags_t flags;
    uint16_t platform_id;
    uint16_t format_id;
    uint64_t cover_size;
    char name[513];
    char serial[129];
} romx_probe_info_t;

#define ROMX_PROBE_INFO_INIT { \
    (uint32_t)sizeof(romx_probe_info_t), UINT32_C(0), UINT16_C(0), \
    UINT16_C(0), UINT64_C(0), { 0 }, { 0 } \
}

typedef struct romx_mutable_write_options {
    uint32_t struct_size;
    uint32_t flags; /* reserved; must be zero; commits are always durable */
    uint64_t data_capacity;
    uint64_t modified_unix_seconds;
    uint32_t io_chunk_size;
    uint32_t reserved;
} romx_mutable_write_options_t;

#define ROMX_MUTABLE_WRITE_OPTIONS_INIT { \
    (uint32_t)sizeof(romx_mutable_write_options_t), UINT32_C(0), \
    UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0) \
}

/* Interoperable, uncompressed SAVE/CHEAT bundle profile. The outer mutable
 * object key selects a consumer-defined destination root; bundle entry paths
 * are normalized UTF-8 paths relative to that root. */
#define ROMX_MUTABLE_BUNDLE_VERSION UINT16_C(1)
#define ROMX_MUTABLE_BUNDLE_PATH_CAPACITY UINT32_C(1024)
#define ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_ENTRIES UINT32_C(4096)
#define ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_SIZE UINT64_C(134217728)

typedef struct romx_mutable_bundle_path_entry {
    uint32_t struct_size;
    uint32_t reserved;
    const char *relative_path;
    const char *source_path;
} romx_mutable_bundle_path_entry_t;

#define ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT { \
    (uint32_t)sizeof(romx_mutable_bundle_path_entry_t), UINT32_C(0), \
    NULL, NULL \
}

typedef struct romx_mutable_bundle_options {
    uint32_t struct_size;
    uint32_t flags; /* reserved; must be zero */
    uint32_t max_entry_count;
    uint32_t max_path_size;
    uint64_t max_bundle_size;
    uint32_t io_chunk_size;
    uint32_t reserved;
} romx_mutable_bundle_options_t;

#define ROMX_MUTABLE_BUNDLE_OPTIONS_INIT { \
    (uint32_t)sizeof(romx_mutable_bundle_options_t), UINT32_C(0), \
    UINT32_C(0), UINT32_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0) \
}

typedef struct romx_mutable_bundle_entry_info {
    uint32_t struct_size;
    uint32_t index;
    uint64_t data_size;
    uint32_t data_crc32;
    uint32_t path_size;
    char path[ROMX_MUTABLE_BUNDLE_PATH_CAPACITY + 1U];
} romx_mutable_bundle_entry_info_t;

#define ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT { \
    (uint32_t)sizeof(romx_mutable_bundle_entry_info_t), UINT32_C(0), \
    UINT64_C(0), UINT32_C(0), UINT32_C(0), { 0 } \
}

/* Strict, versioned STATS JSON profile. Every field other than schema/version
 * is optional and represented by a ROMX_MUTABLE_STATS_HAS_* flag. */
#define ROMX_MUTABLE_STATS_VERSION UINT32_C(1)
#define ROMX_MUTABLE_STATS_MAX_JSON_SIZE UINT32_C(16384)
#define ROMX_MUTABLE_STATS_MAX_SAFE_INTEGER UINT64_C(9007199254740991)

typedef uint32_t romx_mutable_stats_flags_t;
#define ROMX_MUTABLE_STATS_HAS_PLAY_TIME          UINT32_C(0x00000001)
#define ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT       UINT32_C(0x00000002)
#define ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED       UINT32_C(0x00000004)
#define ROMX_MUTABLE_STATS_HAS_LAST_PLAYED        UINT32_C(0x00000008)
#define ROMX_MUTABLE_STATS_HAS_FAVORITE           UINT32_C(0x00000010)
#define ROMX_MUTABLE_STATS_HAS_COMPLETED          UINT32_C(0x00000020)
#define ROMX_MUTABLE_STATS_HAS_COMPLETION_PERCENT UINT32_C(0x00000040)
#define ROMX_MUTABLE_STATS_HAS_ACHIEVEMENTS       UINT32_C(0x00000080)
#define ROMX_MUTABLE_STATS_HAS_HARDCORE_UNLOCKED  UINT32_C(0x00000100)
#define ROMX_MUTABLE_STATS_FLAGS_MASK             UINT32_C(0x000001ff)

typedef struct romx_mutable_stats {
    uint32_t struct_size;
    romx_mutable_stats_flags_t flags;
    uint64_t play_time_seconds;
    uint64_t launch_count;
    uint64_t first_played_unix_seconds;
    uint64_t last_played_unix_seconds;
    uint32_t favorite;
    uint32_t completed;
    uint32_t completion_percent;
    uint32_t reserved;
    uint64_t achievements_unlocked;
    uint64_t achievements_total;
    uint64_t achievements_hardcore_unlocked;
} romx_mutable_stats_t;

#define ROMX_MUTABLE_STATS_INIT { \
    (uint32_t)sizeof(romx_mutable_stats_t), UINT32_C(0), \
    UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
    UINT32_C(0), UINT32_C(0), UINT32_C(0), UINT32_C(0), \
    UINT64_C(0), UINT64_C(0), UINT64_C(0) \
}

ROMX_API const char *romx_result_string(romx_result_t result);

ROMX_API const char *romx_platform_name(uint16_t platform_id);
ROMX_API const char *romx_launch_format_name(uint16_t launch_format_id);
ROMX_API const char *romx_file_format_name(uint16_t format_id);

ROMX_API romx_result_t romx_reader_open_path(
    const char *utf8_path,
    const romx_reader_options_t *options,
    romx_reader_t **out_reader,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_open_io(
    const romx_io_t *io,
    const romx_reader_options_t *options,
    romx_reader_t **out_reader,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_info(
    const romx_reader_t *reader,
    romx_info_t *info,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_entry_count(
    const romx_reader_t *reader,
    uint32_t *count,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_entry(
    const romx_reader_t *reader,
    uint32_t index,
    romx_entry_info_t *entry,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_entrypoint(
    const romx_reader_t *reader,
    romx_entry_info_t *entry,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_find_entry(
    const romx_reader_t *reader,
    const char *virtual_path,
    romx_entry_info_t *entry,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_read_entry(
    const romx_reader_t *reader,
    uint32_t index,
    uint64_t entry_offset,
    void *buffer,
    uint64_t buffer_size,
    uint64_t *bytes_read,
    romx_error_t *error);

/* A VFS cursor borrows its reader. The reader must outlive the cursor. */
ROMX_API romx_result_t romx_vfs_file_open(
    const romx_reader_t *reader,
    const char *virtual_path,
    romx_vfs_file_t **out_file,
    romx_error_t *error);

ROMX_API romx_result_t romx_vfs_file_open_entrypoint(
    const romx_reader_t *reader,
    romx_vfs_file_t **out_file,
    romx_error_t *error);

ROMX_API romx_result_t romx_vfs_file_get_size(
    const romx_vfs_file_t *file,
    uint64_t *size,
    romx_error_t *error);

ROMX_API romx_result_t romx_vfs_file_tell(
    const romx_vfs_file_t *file,
    uint64_t *position,
    romx_error_t *error);

ROMX_API romx_result_t romx_vfs_file_seek(
    romx_vfs_file_t *file,
    int64_t offset,
    romx_payload_seek_position_t origin,
    uint64_t *new_position,
    romx_error_t *error);

ROMX_API romx_result_t romx_vfs_file_read(
    romx_vfs_file_t *file,
    void *buffer,
    uint64_t size,
    uint64_t *bytes_read,
    romx_error_t *error);

ROMX_API void romx_vfs_file_close(romx_vfs_file_t *file);

/* Enumeration is explicit and may scan object bytes to validate data CRC32.
 * Only committed ACTIVE objects are returned. */
ROMX_API romx_result_t romx_reader_get_mutable_status(
    const romx_reader_t *reader,
    romx_mutable_status_t *status,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_mutable_object_count(
    const romx_reader_t *reader,
    uint32_t *count,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_mutable_object(
    const romx_reader_t *reader,
    uint32_t active_index,
    romx_mutable_object_info_t *object,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_find_mutable_object(
    const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    romx_mutable_object_info_t *object,
    romx_error_t *error);

/* A mutable cursor borrows its reader and validates data CRC32 before open. */
ROMX_API romx_result_t romx_mutable_file_open(
    const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    romx_mutable_file_t **out_file,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_file_get_size(
    const romx_mutable_file_t *file,
    uint64_t *size,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_file_tell(
    const romx_mutable_file_t *file,
    uint64_t *position,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_file_seek(
    romx_mutable_file_t *file,
    int64_t offset,
    romx_payload_seek_position_t origin,
    uint64_t *new_position,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_file_read(
    romx_mutable_file_t *file,
    void *buffer,
    uint64_t size,
    uint64_t *bytes_read,
    romx_error_t *error);

ROMX_API void romx_mutable_file_close(romx_mutable_file_t *file);

ROMX_API romx_result_t romx_reader_read_region(
    const romx_reader_t *reader,
    romx_region_t region,
    uint64_t region_offset,
    void *buffer,
    uint64_t buffer_size,
    uint64_t *bytes_read,
    romx_error_t *error);

/*
 * Creates a borrowed, read-only virtual file containing only the RIDX entrypoint.
 * Virtual offset zero maps to that entry's data_offset and get_size reports
 * only that entry's data_size. The returned callbacks never expose metadata,
 * cover, or footer bytes. The reader must outlive every use of out_io.
 */
ROMX_API romx_result_t romx_reader_get_payload_io(
    const romx_reader_t *reader,
    romx_io_t *out_io,
    romx_error_t *error);

/* Opens an independent read-only cursor over the RIDX entrypoint. The handle owns
 * its reader and its current offset, so multiple handles may be used by
 * concurrent callers without sharing cursor state. Reads are bounded to the
 * entrypoint and return regular-file EOF semantics. Immutable SHA-256 remains
 * opt-in through ROMX_PAYLOAD_FILE_VALIDATE_IMMUTABLE_SHA256. */
ROMX_API romx_result_t romx_payload_file_open_path(
    const char *utf8_path,
    const romx_reader_options_t *reader_options,
    const romx_payload_file_options_t *options,
    romx_payload_file_t **out_file,
    romx_error_t *error);

ROMX_API romx_result_t romx_payload_file_get_size(
    const romx_payload_file_t *file,
    uint64_t *size,
    romx_error_t *error);

ROMX_API romx_result_t romx_payload_file_tell(
    const romx_payload_file_t *file,
    uint64_t *position,
    romx_error_t *error);

ROMX_API romx_result_t romx_payload_file_seek(
    romx_payload_file_t *file,
    int64_t offset,
    romx_payload_seek_position_t position,
    uint64_t *new_position,
    romx_error_t *error);

ROMX_API romx_result_t romx_payload_file_read(
    romx_payload_file_t *file,
    void *buffer,
    uint64_t size,
    uint64_t *bytes_read,
    romx_error_t *error);

ROMX_API void romx_payload_file_close(romx_payload_file_t *file);

/*
 * Creates an independently owned, read-only mapping of the ROM payload.
 * The mapping remains valid after the reader is closed and must be released
 * with romx_payload_mapping_close(). Implementations isolate partial boundary
 * pages so bytes belonging to metadata, cover, or the footer are not exposed.
 * Readers opened from custom romx_io_t sources may return ROMX_E_UNSUPPORTED.
 */
ROMX_API romx_result_t romx_reader_map_payload(
    const romx_reader_t *reader,
    romx_payload_mapping_t **out_mapping,
    romx_error_t *error);

ROMX_API const void *romx_payload_mapping_data(
    const romx_payload_mapping_t *mapping);

ROMX_API uint64_t romx_payload_mapping_size(
    const romx_payload_mapping_t *mapping);

ROMX_API void romx_payload_mapping_close(
    romx_payload_mapping_t *mapping);

ROMX_API romx_result_t romx_reader_copy_region(
    const romx_reader_t *reader,
    romx_region_t region,
    const romx_sink_t *sink,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_validate(
    const romx_reader_t *reader,
    romx_validate_flags_t flags,
    romx_validation_report_t *report,
    romx_error_t *error);

ROMX_API romx_result_t romx_metadata_open(
    const romx_reader_t *reader,
    romx_metadata_t **out_metadata,
    romx_error_t *error);

ROMX_API void romx_metadata_close(romx_metadata_t *metadata);

ROMX_API romx_result_t romx_metadata_copy_json(
    const romx_metadata_t *metadata,
    void *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API romx_result_t romx_metadata_get_string(
    const romx_metadata_t *metadata,
    const char *key,
    char *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

/* Parses the canonical eight-digit metadata CRC32 into a native value. */
ROMX_API romx_result_t romx_metadata_get_crc32(
    const romx_metadata_t *metadata,
    uint32_t *crc32,
    romx_error_t *error);

ROMX_API romx_result_t romx_metadata_get_value_json(
    const romx_metadata_t *metadata,
    const char *key,
    void *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API romx_result_t romx_extract_payload_path(
    const romx_reader_t *reader,
    const char *utf8_destination_path,
    const romx_extract_options_t *options,
    romx_error_t *error);

ROMX_API romx_result_t romx_extract_payload_cache(
    const romx_reader_t *reader,
    const char *utf8_cache_directory,
    const romx_extract_options_t *options,
    char *result_path,
    uint64_t result_capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API romx_result_t romx_reader_get_cover_info(
    const romx_reader_t *reader,
    romx_cover_info_t *info,
    romx_error_t *error);

ROMX_API romx_result_t romx_extract_cover_path(
    const romx_reader_t *reader,
    const char *utf8_destination_path,
    const romx_extract_options_t *options,
    romx_error_t *error);

ROMX_API romx_result_t romx_writer_write_io_entries(
    const char *utf8_destination_path,
    const romx_writer_io_entry_t *entries,
    uint32_t entry_count,
    const void *metadata_json,
    uint64_t metadata_size,
    const romx_io_t *cover,
    const romx_writer_options_t *options,
    romx_writer_report_t *report,
    romx_error_t *error);

ROMX_API romx_result_t romx_writer_write_path_entries(
    const char *utf8_destination_path,
    const romx_writer_path_entry_t *entries,
    uint32_t entry_count,
    const char *utf8_metadata_path,
    const char *utf8_cover_path,
    const romx_writer_options_t *options,
    romx_writer_report_t *report,
    romx_error_t *error);

ROMX_API romx_result_t romx_probe_open_io(
    const romx_io_t *payload,
    uint16_t format_hint,
    romx_probe_t **out_probe,
    romx_error_t *error);

ROMX_API romx_result_t romx_probe_open_path(
    const char *utf8_payload_path,
    uint16_t format_hint,
    romx_probe_t **out_probe,
    romx_error_t *error);

ROMX_API romx_result_t romx_probe_get_info(
    const romx_probe_t *probe,
    romx_probe_info_t *info,
    romx_error_t *error);

ROMX_API romx_result_t romx_probe_copy_metadata_json(
    const romx_probe_t *probe,
    void *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API romx_result_t romx_probe_copy_cover_png(
    const romx_probe_t *probe,
    void *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API void romx_probe_close(romx_probe_t *probe);

ROMX_API romx_result_t romx_mutable_write_io_path(
    const char *utf8_romx_path,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    const romx_io_t *source,
    const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written_object,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_write_path(
    const char *utf8_romx_path,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    const char *utf8_source_path,
    const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written_object,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_delete_path(
    const char *utf8_romx_path,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_bundle_write_path_entries(
    const char *utf8_romx_path,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    const romx_mutable_bundle_path_entry_t *entries,
    uint32_t entry_count,
    const romx_mutable_bundle_options_t *bundle_options,
    const romx_mutable_write_options_t *write_options,
    romx_mutable_object_info_t *written_object,
    romx_error_t *error);

/* A bundle borrows its reader. Opening validates the complete bundle,
 * including every per-file CRC32, before any entry can be exposed. */
ROMX_API romx_result_t romx_mutable_bundle_open(
    const romx_reader_t *reader,
    romx_mutable_namespace_t object_namespace,
    const char *key,
    const romx_mutable_bundle_options_t *options,
    romx_mutable_bundle_t **out_bundle,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_bundle_get_entry_count(
    const romx_mutable_bundle_t *bundle,
    uint32_t *count,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_bundle_get_entry(
    const romx_mutable_bundle_t *bundle,
    uint32_t index,
    romx_mutable_bundle_entry_info_t *entry,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_bundle_read_entry(
    romx_mutable_bundle_t *bundle,
    uint32_t index,
    uint64_t entry_offset,
    void *buffer,
    uint64_t buffer_size,
    uint64_t *bytes_read,
    romx_error_t *error);

ROMX_API void romx_mutable_bundle_close(romx_mutable_bundle_t *bundle);

ROMX_API romx_result_t romx_mutable_stats_parse_json(
    const void *json,
    uint64_t json_size,
    romx_mutable_stats_t *stats,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_stats_serialize_json(
    const romx_mutable_stats_t *stats,
    void *buffer,
    uint64_t capacity,
    uint64_t *required_size,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_stats_read(
    const romx_reader_t *reader,
    const char *key,
    romx_mutable_stats_t *stats,
    romx_error_t *error);

ROMX_API romx_result_t romx_mutable_stats_write_path(
    const char *utf8_romx_path,
    const char *key,
    const romx_mutable_stats_t *stats,
    const romx_mutable_write_options_t *options,
    romx_mutable_object_info_t *written_object,
    romx_error_t *error);

ROMX_API void romx_reader_close(romx_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif
