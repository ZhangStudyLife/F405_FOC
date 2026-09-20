#include "mt6835.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Independent bitwise reference, polynomial 0x07, initial value 0. */
static void make_frame(uint8_t frame[6], uint32_t raw, uint8_t status)
{
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = raw >> 13;
    frame[3] = raw >> 5;
    frame[4] = (raw << 3) | status;
    uint8_t crc = 0;
    for (unsigned i = 2; i < 5; ++i) {
        crc ^= frame[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 7 : 0));
    }
    frame[5] = crc;
}

int main(void)
{
    uint8_t frame[6], damaged[6];
    const uint32_t boundaries[] = {0, 1, 0xfffff, 0x100000, 0x1ffffe, 0x1fffff};
    for (unsigned i = 0; i < sizeof boundaries / sizeof *boundaries; ++i) {
        make_frame(frame, boundaries[i], 0);
        assert(mt6835_decode(frame) == (float)boundaries[i] * (360.0f / 2097152.0f));
    }
    for (uint32_t raw = 0; raw < 2097152u; raw += 9973u) {
        make_frame(frame, raw, 0);
        assert(mt6835_decode(frame) == (float)raw * (360.0f / 2097152.0f));
        for (unsigned bit = 0; bit < 32; ++bit) {
            memcpy(damaged, frame, sizeof frame);
            damaged[2 + bit / 8] ^= 1u << (bit % 8);
            assert(isnan(mt6835_decode(damaged)));
        }
        for (uint8_t status = 1; status < 8; ++status) {
            make_frame(frame, raw, status);
            assert(isnan(mt6835_decode(frame)));
        }
    }
    /* Captured hardware frame, independently recorded before this refactor. */
    const uint8_t captured[] = {0, 0, 0x3f, 0x54, 0x10, 142};
    assert(mt6835_decode(captured) == 518786.0f * (360.0f / 2097152.0f));
    puts("PASS: angle boundaries, reference CRC, all single-bit corruptions, sensor faults, captured frame");
    return 0;
}
