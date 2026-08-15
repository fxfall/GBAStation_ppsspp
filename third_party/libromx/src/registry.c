#include <romx/romx.h>

const char *romx_platform_name(uint16_t value)
{
    switch (value) {
    case 0x0000: return "UNSPECIFIED";
    case 0x0001: return "GAME_BOY";
    case 0x0002: return "GAME_BOY_COLOR";
    case 0x0003: return "GAME_BOY_ADVANCE";
    case 0x0004: return "NES";
    case 0x0005: return "SNES";
    case 0x0006: return "NINTENDO_64";
    case 0x0007: return "NINTENDO_DS";
    case 0x0008: return "NINTENDO_3DS";
    case 0x0010: return "MASTER_SYSTEM";
    case 0x0011: return "GAME_GEAR";
    case 0x0012: return "MEGA_DRIVE";
    case 0x0013: return "MEGA_DRIVE_32X";
    case 0x0014: return "SEGA_CD";
    case 0x0015: return "SEGA_SATURN";
    case 0x0016: return "DREAMCAST";
    case 0x0020: return "PC_ENGINE";
    case 0x0021: return "PC_ENGINE_CD";
    case 0x0030: return "PLAYSTATION";
    case 0x0031: return "PLAYSTATION_2";
    case 0x0032: return "PSP";
    case 0x0040: return "GAMECUBE";
    case 0x0041: return "WII";
    case 0x0050: return "ARCADE";
    case 0x0060: return "SCUMMVM";
    case 0x0061: return "DOS";
    case 0x0062: return "AMIGA";
    default: return NULL;
    }
}

const char *romx_launch_format_name(uint16_t value)
{
    switch (value) {
    case 0x0000: return "UNSPECIFIED";
    case 0x0001: return "RAW_SINGLE_FILE";
    case 0x0002: return "CUE";
    case 0x0003: return "GDI";
    case 0x0004: return "M3U";
    case 0x0005: return "CCD";
    case 0x0006: return "MDS";
    case 0x0007: return "TOC";
    case 0x0008: return "DIRECTORY";
    case 0x0009: return "ROMSET";
    case 0x000a: return "SPLIT_FILE_SET";
    default: return NULL;
    }
}

const char *romx_file_format_name(uint16_t value)
{
    switch (value) {
    case 0x0000: return "UNKNOWN";
    case 0x0001: return "GB";
    case 0x0002: return "GBC";
    case 0x0003: return "GBA";
    case 0x0004: return "NES";
    case 0x0005: return "UNF";
    case 0x0006: return "UNIF";
    case 0x0007: return "FDS";
    case 0x0008: return "SFC";
    case 0x0009: return "SMC";
    case 0x000a: return "NDS";
    case 0x000b: return "N3DS";
    case 0x000c: return "CCI";
    case 0x000d: return "CXI";
    case 0x000e: return "APP";
    case 0x0010: return "ISO";
    case 0x0011: return "CSO";
    case 0x0012: return "ZSO";
    case 0x0013: return "CHD";
    case 0x0014: return "PBP";
    case 0x0015: return "CDI";
    case 0x0016: return "GCM";
    case 0x0017: return "WBFS";
    case 0x0018: return "RVZ";
    case 0x0019: return "WIA";
    case 0x001a: return "WAD";
    case 0x0020: return "CUE";
    case 0x0021: return "GDI";
    case 0x0022: return "M3U";
    case 0x0023: return "CCD";
    case 0x0024: return "MDS";
    case 0x0025: return "TOC";
    case 0x0030: return "BIN";
    case 0x0031: return "WAV";
    case 0x0032: return "FLAC";
    case 0x0033: return "IMG";
    case 0x0034: return "MDF";
    case 0x0040: return "SBI";
    case 0x0041: return "SUB";
    case 0x0042: return "ECM";
    case 0x0050: return "Z64";
    case 0x0051: return "N64";
    case 0x0052: return "V64";
    case 0x0060: return "MD";
    case 0x0061: return "GEN";
    case 0x0062: return "SMD";
    case 0x0063: return "X32";
    case 0x0064: return "SMS";
    case 0x0065: return "GG";
    case 0x0066: return "PCE";
    case 0x0070: return "ELF";
    case 0x0071: return "PRX";
    case 0x0080: return "MSU";
    case 0x0081: return "PCM";
    case 0x0090: return "ROMX_LAUNCH_DESCRIPTOR";
    default: return NULL;
    }
}
