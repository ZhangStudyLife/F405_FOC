#include "adc_test.h"
#include "bsp_adc.h"
#include "bsp_time.h"
#include "justfloat.h"
#include "stm32f4xx_hal.h"

/* Sampling diagnostics; no commands can enable power outputs. */
volatile struct {
    uint32_t started;
    uint32_t samples;
    uint32_t tx_rejected;
    uint32_t period_min_cycles;
    uint32_t period_max_cycles;
    uint32_t isr_max_cycles;
    uint32_t incomplete_pairs;
} g_adc_test;
static uint32_t s_previous_cycles;

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
    TIM8->PSC = 0u; /* Center-aligned: 168 MHz / (2 * 4200) = 20 kHz, 50 us. */
    /* CCR4=4100 is a bring-up trigger, not a validated running-current window.
       FOC must keep B/C low sides conducting throughout the sampling aperture,
       after dead time and switching/amplifier settling; otherwise adjust PWM
       or the trigger. All six gate pins remain low in this test. */
    TIM8->CNT = 0u;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0u;
    DBGMCU->APB2FZ |= DBGMCU_APB2_FZ_DBG_TIM8_STOP;

    g_adc_test.period_min_cycles = UINT32_MAX;
    g_adc_test.started = bsp_adc_start() ? 1u : 0u;
    if (g_adc_test.started != 0u) {
        TIM8->BDTR |= TIM_BDTR_MOE; /* CH4 only; gate pins remain GPIO low. */
        TIM8->CR1 |= TIM_CR1_CEN;
    }
}

void adc_test_isr(void)
{
    uint32_t start = DWT->CYCCNT;
    if (!bsp_adc_read()) {
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
    g_adc_test.samples++;
    /* Only telemetry is decimated to 1 kHz; all voltages update at 20 kHz. */
    if (g_adc_test.samples % 20u == 0u &&
        !justfloat_send(adc_m1.b_voltage, adc_m1.c_voltage, adc_bus_voltage)) g_adc_test.tx_rejected++;
    uint32_t elapsed = DWT->CYCCNT - start;
    if (elapsed > g_adc_test.isr_max_cycles) g_adc_test.isr_max_cycles = elapsed;
}
