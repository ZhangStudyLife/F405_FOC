#include "bsp_uart.h"
#include "usart.h"
#include <string.h>

static uint8_t s_tx[2][256];
static uint8_t s_rx[128];
static volatile uint16_t s_used;
static volatile uint8_t s_fill, s_busy;
static volatile uint8_t s_rx_head, s_rx_tail;
volatile bsp_uart_stats_t g_uart_stats;

void bsp_uart_init(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    /* DMA receives bytes while the priority-1 FOC sample path is running. */
    DMA1_Stream5->PAR = (uint32_t)&USART2->DR;
    DMA1_Stream5->M0AR = (uint32_t)s_rx;
    DMA1_Stream5->NDTR = sizeof s_rx;
    DMA1_Stream5->CR = DMA_SxCR_CHSEL_2 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
        DMA_SxCR_PL_1 | DMA_SxCR_HTIE | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_EN;
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 2u, 0u);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    HAL_NVIC_SetPriority(USART2_IRQn, 2u, 0u);
    USART2->CR3 |= USART_CR3_DMAR | USART_CR3_EIE;
    USART2->CR1 |= USART_CR1_IDLEIE;
}

bool bsp_uart_write(const void *data, size_t size)
{
    if (data == NULL || size == 0u || size > sizeof s_tx[0]) return false;
    /* Only app_sample produces TX data. Allow priority-0 PWM updates during copy. */
    if (size > sizeof s_tx[0] - s_used) {
        g_uart_stats.tx_rejected++;
        return false;
    }
    memcpy(s_tx[s_fill] + s_used, data, size);
    s_used += (uint16_t)size;
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

void bsp_uart_rx_irq(void)
{
    uint32_t flags = DMA1->HISR;
    DMA1->HIFCR = 0xf40u; /* Stream 5 only; TX uses stream 6, SPI3 TX uses stream 7. */
    uint8_t head = (sizeof s_rx - DMA1_Stream5->NDTR) & 127u;
    uint8_t received = (head - s_rx_head) & 127u;
    if ((flags & (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)) ||
        (flags & (DMA_HISR_HTIF5 | DMA_HISR_TCIF5)) == (DMA_HISR_HTIF5 | DMA_HISR_TCIF5) ||
        received > ((s_rx_tail - s_rx_head - 1u) & 127u)) {
        g_uart_stats.rx_lost++;
        s_rx_tail = head;
    }
    s_rx_head = head;
}

void bsp_uart_irq(void)
{
    uint32_t status = USART2->SR;
    if (status & (USART_SR_IDLE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
        (void)USART2->DR; /* SR then DR clears IDLE and receive errors. */
        bsp_uart_rx_irq();
        if (status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
            g_uart_stats.rx_errors++;
        }
    }
    /* Handle TC only: DMA owns receive data, not the HAL RX dispatcher. */
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
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    while (count < size && s_rx_tail != s_rx_head) {
        out[count++] = s_rx[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) & 127u;
    }
    __set_PRIMASK(mask);
    return count;
}
