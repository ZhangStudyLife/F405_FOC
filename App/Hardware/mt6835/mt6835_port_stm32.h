#ifndef APP_HARDWARE_MT6835_PORT_STM32_H
#define APP_HARDWARE_MT6835_PORT_STM32_H

#include <stdbool.h>
#include <stdint.h>

extern volatile float mt6835_angle_deg;
extern volatile uint32_t mt6835_errors;

/* Fixed board: SPI3 mode 3 / 10.5 MHz, PA0 CS, DMA1 streams 0/5 channel 0. */
bool mt6835_init(void);
void mt6835_start(void);
void mt6835_finish(void);

#endif
