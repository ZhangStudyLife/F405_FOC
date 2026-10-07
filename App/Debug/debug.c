#include "debug.h"
#include "app.h"
#include "foc.h"
#include "bsp_uart.h"
#include "bsp_usb.h"

void debug_init(void)
{
#if DEBUG_UART_ENABLE
    bsp_uart_init();
#endif
}

void debug_write(const void *data, size_t size)
{
#if DEBUG_USB_ENABLE
    bsp_usb_write(data, size);
#endif
#if DEBUG_UART_ENABLE
    bsp_uart_write(data, size);
#endif
}

void debug_tick(void)
{
#if DEBUG_UART_ENABLE
    bsp_uart_tick();
#endif
}

uint32_t debug_tx_rejected(void)
{
#if DEBUG_CMD_SOURCE == DEBUG_CMD_USB
    return g_usb_stats.tx_rejected;
#elif DEBUG_CMD_SOURCE == DEBUG_CMD_UART
    return g_uart_stats.tx_rejected;
#else
    return 0;
#endif
}

void debug_poll(void)
{
#if DEBUG_USB_ENABLE || DEBUG_UART_ENABLE
    uint8_t data[64];
#endif
#if DEBUG_CMD_SOURCE != DEBUG_CMD_NONE
    static char line[64];
    static unsigned length;
    static bool overflow;
    size_t count;
#endif
#if DEBUG_USB_ENABLE
    bsp_usb_poll();
#if DEBUG_CMD_SOURCE == DEBUG_CMD_USB
    static uint32_t session;
    if (session != g_usb_stats.session) {
        session = g_usb_stats.session;
        length = 0; overflow = false;
    }
    count = bsp_usb_read(data, sizeof data);
#else
    bsp_usb_read(data, sizeof data);
#endif
#endif
#if DEBUG_UART_ENABLE
#if DEBUG_CMD_SOURCE == DEBUG_CMD_UART
    static uint32_t rx_errors, rx_dma_errors;
    if (rx_dma_errors != g_uart_stats.rx_dma_errors) {
        rx_dma_errors = g_uart_stats.rx_dma_errors;
        app_fault(FOC_COMM);
    }
    count = bsp_uart_read(data, sizeof data);
#else
    bsp_uart_read(data, sizeof data);
#endif
#endif
#if DEBUG_CMD_SOURCE != DEBUG_CMD_NONE
    for (size_t i = 0; i < count; ++i) {
#if DEBUG_CMD_SOURCE == DEBUG_CMD_USB
        if (session != g_usb_stats.session) { length = 0; overflow = false; break; }
#else
        uint32_t errors = g_uart_stats.rx_errors + g_uart_stats.rx_lost;
        if (errors != rx_errors) { overflow = true; rx_errors = errors; }
#endif
        uint8_t ch = data[i];
        if (ch == '\r' || ch == '\n') {
            line[length] = 0;
            if (overflow) app_command("");
            else if (length) app_command(line);
            length = 0; overflow = false;
        } else if (ch >= 32 && ch < 127 && length < sizeof line - 1) line[length++] = (char)ch;
        else overflow = true;
    }
#endif
}
