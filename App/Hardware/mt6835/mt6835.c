#include "mt6835.h"
#include <math.h>

float mt6835_decode(const uint8_t frame[6])
{
    static const uint8_t table[16] = {
        0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
        0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d
    };
    uint8_t crc = 0u;
    for (unsigned i = 2u; i < 5u; ++i) {
        crc ^= frame[i];
        crc = (uint8_t)((crc << 4) ^ table[crc >> 4]);
        crc = (uint8_t)((crc << 4) ^ table[crc >> 4]);
    }
    if (crc != frame[5] || (frame[4] & 7u)) return NAN;
    uint32_t raw = ((uint32_t)frame[2] << 13) | ((uint32_t)frame[3] << 5) | (frame[4] >> 3);
    return (float)raw * (360.0f / 2097152.0f);
}
