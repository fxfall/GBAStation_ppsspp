#include "romx_internal.h"

uint32_t romx_crc32_begin(void)
{
    return UINT32_C(0xffffffff);
}

uint32_t romx_crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned int bit;
        crc ^= (uint32_t)data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & UINT32_C(1));
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

uint32_t romx_crc32_finish(uint32_t crc)
{
    return crc ^ UINT32_C(0xffffffff);
}
