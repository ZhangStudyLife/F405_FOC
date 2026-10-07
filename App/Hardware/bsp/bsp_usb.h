#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t tx_rejected, tx_peak, session; } bsp_usb_stats_t;
extern volatile bsp_usb_stats_t g_usb_stats;

/* One sample-IRQ producer, one foreground reader. Copies all or none, at most 256 B. */
bool bsp_usb_write(const void *data, size_t size);
size_t bsp_usb_read(void *data, size_t size);
void bsp_usb_poll(void);

/* CDC/PCD IRQ callbacks; reset only after the stack has stopped old transfers. */
void bsp_usb_reset(void);
void bsp_usb_open(bool opened);
void bsp_usb_suspend(void);
void bsp_usb_received(uint8_t *data, uint32_t size);
void bsp_usb_transmitted(void);
