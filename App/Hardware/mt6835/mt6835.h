#ifndef APP_HARDWARE_MT6835_H
#define APP_HARDWARE_MT6835_H

#include <stdint.h>

/* 6-byte burst reply -> mechanical degrees [0, 360), or NaN on CRC/status error. */
float mt6835_decode(const uint8_t frame[6]);

#endif
