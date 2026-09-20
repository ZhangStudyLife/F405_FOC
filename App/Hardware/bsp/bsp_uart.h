#ifndef APP_HARDWARE_BSP_UART_H
#define APP_HARDWARE_BSP_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t tx_rejected, dma_errors, rx_lost, rx_errors;
} bsp_uart_stats_t;
extern volatile bsp_uart_stats_t g_uart_stats;

/* Call once after MX_USART2_UART_Init. No other code may use huart2 TX/RX. */
bool bsp_uart_init(void);
/* Copies the complete message or returns false immediately; maximum 256 bytes.
 * true means queued, not delivered. DMA errors are counted; no automatic replay.
 * Call from thread/ordinary IRQ context, not NMI. read() has one consumer. */
bool bsp_uart_write(const void *data, size_t size);
size_t bsp_uart_read(void *data, size_t size);
uint32_t bsp_uart_millis(void);

/* Board hooks: SysTick after HAL_IncTick, and USART2 IRQ respectively. */
void bsp_uart_tick(void);
void bsp_uart_irq(void);

#endif
