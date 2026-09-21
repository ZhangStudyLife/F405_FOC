#include "bsp_uart.h"
#include "usart.h"
#include <string.h>

static uint8_t s_tx[2][256];
static uint8_t s_rx[128];
static volatile uint16_t s_used;
static volatile uint8_t s_fill, s_busy;
static volatile uint8_t s_rx_head, s_rx_tail;
volatile bsp_uart_stats_t g_uart_stats;

bool bsp_uart_init(void)
{
    huart2.Init.BaudRate = 3500000u;
    huart2.Init.OverSampling = UART_OVERSAMPLING_8;
    if (HAL_UART_Init(&huart2) != HAL_OK) return false;
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    /* At 3.5 Mbps a byte arrives every 2.86 us. RX preempts FOC arithmetic. */
    HAL_NVIC_SetPriority(USART2_IRQn, 0u, 0u);
    USART2->CR1 |= USART_CR1_RXNEIE;
    return true;
}

bool bsp_uart_write(const void *data, size_t size)
{
    if (data == NULL || size == 0u || size > sizeof s_tx[0]) return false;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (size > sizeof s_tx[0] - s_used) {
        g_uart_stats.tx_rejected++;
        __set_PRIMASK(mask);
        return false;
    }
    memcpy(s_tx[s_fill] + s_used, data, size);
    s_used += (uint16_t)size;
    __set_PRIMASK(mask);
    return true;
}

void bsp_uart_tick(void)
{
    if (s_busy || s_used == 0u) return;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint16_t size = s_used;
    uint8_t sending = s_fill;
    s_fill ^= 1u;
    s_used = 0u;
    s_busy = 1u;
    __set_PRIMASK(mask); /* The producer can now fill the other buffer. */
    if (HAL_UART_Transmit_DMA(&huart2, s_tx[sending], size) != HAL_OK) {
        s_busy = 0u;
        g_uart_stats.dma_errors++;
    } else {
        /* No half-transfer work is needed. */
        __HAL_DMA_DISABLE_IT(huart2.hdmatx, DMA_IT_HT);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart2 && (uart->ErrorCode & HAL_UART_ERROR_DMA)) {
        g_uart_stats.dma_errors++;
        s_busy = 0u;
    }
}

void bsp_uart_irq(void)
{
    uint32_t status = USART2->SR;
    if (status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
        uint8_t byte = (uint8_t)USART2->DR; /* SR then DR clears receive errors. */
        if (status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
            g_uart_stats.rx_errors++;
        } else {
            uint8_t next = (s_rx_head + 1u) & 127u;
            if (next == s_rx_tail) g_uart_stats.rx_lost++;
            else {
                s_rx[s_rx_head] = byte;
                s_rx_head = next;
            }
        }
    }
    /* Handle TC only: HAL RX dispatch must not race our own RXNE handling. */
    if ((USART2->SR & USART_SR_TC) && (USART2->CR1 & USART_CR1_TCIE)) {
        USART2->CR1 &= ~USART_CR1_TCIE;
        huart2.gState = HAL_UART_STATE_READY;
        s_busy = 0u;
    }
}

size_t bsp_uart_read(void *data, size_t size)
{
    uint8_t *out = data;
    size_t count = 0u;
    if (out == NULL) return 0u;
    while (count < size && s_rx_tail != s_rx_head) {
        out[count++] = s_rx[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) & 127u;
    }
    return count;
}

uint32_t bsp_uart_millis(void) { return HAL_GetTick(); }
