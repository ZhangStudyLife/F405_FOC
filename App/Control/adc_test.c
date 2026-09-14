#include "adc_test.h"
#include "bsp_adc.h"
#include "bsp_time.h"
#include "scope.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

/* SWD observation only; no commands can enable power outputs. */
volatile struct {
    uint32_t started;
    uint32_t samples;
    uint32_t raw[4];                 /* PC3, PC2, PA4, PA6 */
    uint32_t period_min_cycles;
    uint32_t period_max_cycles;
    uint32_t isr_max_cycles;
    uint32_t incomplete_pairs;
} g_adc_test;
static scope_t s_adc_scope;
static uint32_t s_previous_cycles;
static uint8_t s_uart_tx[16];
static uint32_t s_uart_next_ms;
static uint16_t s_uart_sequence;

void adc_test_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    /* Disconnect all six gate pins from TIM8; latch low before output mode. */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8, GPIO_PIN_RESET);
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8;
    HAL_GPIO_Init(GPIOC, &gpio);
    TIM8->BDTR &= ~TIM_BDTR_MOE;
    TIM8->CCER = TIM_CCER_CC4E;
    TIM8->CNT = 0u;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0u;
    DBGMCU->APB2FZ |= DBGMCU_APB2_FZ_DBG_TIM8_STOP;

    scope_init(&s_adc_scope, 20000u);
    g_adc_test.period_min_cycles = UINT32_MAX;
    s_uart_next_ms = HAL_GetTick() + 1u;
    g_adc_test.started = bsp_adc_start() ? 1u : 0u;
    if (g_adc_test.started != 0u) {
        TIM8->BDTR |= TIM_BDTR_MOE; /* CH4 only; gate pins remain GPIO low. */
        TIM8->CR1 |= TIM_CR1_CEN;
    }
}

void adc_test_poll(void)
{
    uint32_t now = HAL_GetTick();
    uint16_t sequence;

    if ((int32_t)(now - s_uart_next_ms) < 0) return;
    s_uart_next_ms = now + 1u;
    if (HAL_UART_GetState(&huart2) != HAL_UART_STATE_READY) return;

    sequence = s_uart_sequence++;
    s_uart_tx[0] = 0xA5u;
    s_uart_tx[1] = 0x5Au;
    s_uart_tx[2] = (uint8_t)sequence;
    s_uart_tx[3] = (uint8_t)(sequence >> 8);
    for (uint32_t i = 0u; i < 4u; ++i) {
        uint16_t value = (uint16_t)g_adc_test.raw[i];
        s_uart_tx[4u + 2u * i] = (uint8_t)value;
        s_uart_tx[5u + 2u * i] = (uint8_t)(value >> 8);
    }
    s_uart_tx[12] = (uint8_t)g_adc_test.samples;
    s_uart_tx[13] = (uint8_t)(g_adc_test.samples >> 8);
    s_uart_tx[14] = (uint8_t)(g_adc_test.samples >> 16);
    s_uart_tx[15] = (uint8_t)(g_adc_test.samples >> 24);
    (void)HAL_UART_Transmit_DMA(&huart2, s_uart_tx, sizeof s_uart_tx);
}

void adc_test_isr(void)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t timer_count = TIM8->CNT;
    bsp_adc_frame_t frame;
    if (!bsp_adc_read(&frame)) {
        g_adc_test.incomplete_pairs++;
        bsp_adc_stop();
        TIM8->CR1 &= ~TIM_CR1_CEN;
        return;
    }
    uint32_t period = start - s_previous_cycles;
    s_previous_cycles = start;
    if (g_adc_test.samples != 0u) {
        if (period < g_adc_test.period_min_cycles) g_adc_test.period_min_cycles = period;
        if (period > g_adc_test.period_max_cycles) g_adc_test.period_max_cycles = period;
    }
    g_adc_test.raw[0] = frame.m[0];
    g_adc_test.raw[1] = frame.s[0];
    g_adc_test.raw[2] = frame.m[1];
    g_adc_test.raw[3] = frame.s[1];
    int32_t values[SCOPE_CHANNELS] = {
        frame.m[0], frame.s[0], frame.m[1], frame.s[1], (int32_t)period, (int32_t)timer_count
    };
    scope_push(&s_adc_scope, values);
    g_adc_test.samples++;
    uint32_t elapsed = DWT->CYCCNT - start;
    if (elapsed > g_adc_test.isr_max_cycles) g_adc_test.isr_max_cycles = elapsed;
}
