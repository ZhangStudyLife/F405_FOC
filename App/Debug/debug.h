#pragma once
#include <stddef.h>
#include <stdint.h>

#define DEBUG_USB_ENABLE  1
#define DEBUG_UART_ENABLE 0
#define DEBUG_CMD_NONE    0
#define DEBUG_CMD_USB     1
#define DEBUG_CMD_UART    2
#define DEBUG_CMD_SOURCE  DEBUG_CMD_USB
#define DEBUG_TELEMETRY_HZ 1000u

#if DEBUG_CMD_SOURCE < DEBUG_CMD_NONE || DEBUG_CMD_SOURCE > DEBUG_CMD_UART || \
    (DEBUG_CMD_SOURCE == DEBUG_CMD_USB && !DEBUG_USB_ENABLE) || \
    (DEBUG_CMD_SOURCE == DEBUG_CMD_UART && !DEBUG_UART_ENABLE)
#error Invalid debug command source
#endif
#if DEBUG_TELEMETRY_HZ == 0 || 20000u % DEBUG_TELEMETRY_HZ
#error Debug telemetry rate must divide 20000 Hz
#endif

void debug_init(void);
void debug_write(const void *data, size_t size);
void debug_tick(void);
void debug_poll(void);
uint32_t debug_tx_rejected(void);
