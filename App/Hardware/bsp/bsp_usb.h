#ifndef APP_HARDWARE_BSP_USB_H
#define APP_HARDWARE_BSP_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t tx_bytes, tx_rejected, tx_discarded, tx_high_water;
    uint32_t rx_bytes, sessions, interruptions;
    bool overflow;
} bsp_usb_stats_t;
extern volatile bsp_usb_stats_t g_usb_stats;

/* One producer only: app_sample ISR. Copies an entire frame, never waits.
   Overflow latches until USB reset/re-enumeration; inspect g_usb_stats. */
bool bsp_usb_ready(void);
bool bsp_usb_write(const void *data, size_t size);
/* Foreground only; USB IRQ is excluded briefly, acquisition IRQs stay enabled. */
void bsp_usb_poll(void);
size_t bsp_usb_read(void *data, size_t size);

/* CDC callbacks, USB IRQ only (lower priority than the producer). */
void bsp_usb_reset(void);
void bsp_usb_control(bool open);
void bsp_usb_suspend(void);
void bsp_usb_received(uint8_t *data, uint32_t size);
void bsp_usb_transmitted(void);

#endif
