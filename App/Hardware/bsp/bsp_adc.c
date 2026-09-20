#include "bsp_adc.h"
#include "stm32f4xx_hal.h"
#include "bsp_time.h"

volatile bsp_adc_m1_t adc_m1;
volatile float adc_bus_voltage;

bool bsp_adc_start(void)
{
    /* Configure while both ADCs are off: rank 1 samples B/C simultaneously.
       Rank 2 samples VBUS on both ADCs to keep sequence timing identical;
       only ADC1's bus result is used. */
    ADC1->CR2 = 0u;
    ADC2->CR2 = 0u;
    ADC->CCR = ADC_DUALMODE_INJECSIMULT | ADC_CLOCK_SYNC_PCLK_DIV4;
    ADC1->CR1 = ADC_CR1_SCAN;
    ADC2->CR1 = ADC_CR1_SCAN;
    ADC1->JSQR = ADC_JSQR_JL_0 | (13u << 10) | (6u << 15);
    ADC2->JSQR = ADC_JSQR_JL_0 | (12u << 10) | (6u << 15);
    /* PCLK2 / 4 = 21 MHz; 28 sampling + 12 conversion cycles = 1.905 us. */
    ADC1->SMPR1 = ADC_SAMPLETIME_28CYCLES << 9;
    ADC2->SMPR1 = ADC_SAMPLETIME_28CYCLES << 6;
    /* VBUS: 39k || 2.2k = 2.08k, shunted by 100 nF; use 15 sample cycles.
       Both ranks: (28+12+15+12)/21 MHz = 3.190 us per 50 us period. */
    ADC1->SMPR2 = ADC_SAMPLETIME_15CYCLES << 18;
    ADC2->SMPR2 = ADC_SAMPLETIME_15CYCLES << 18;
    ADC1->SR = 0u;
    ADC2->SR = 0u;
    ADC2->CR2 = ADC_CR2_ADON; /* Slave: no independent trigger or auto-injection. */
    ADC1->CR2 = ADC_EXTERNALTRIGINJECCONV_T8_CC4 |
                 ADC_EXTERNALTRIGINJECCONVEDGE_RISING | ADC_CR2_ADON;
    bsp_time_delay_us(3u);
    NVIC_ClearPendingIRQ(ADC_IRQn);
    ADC1->CR1 |= ADC_CR1_JEOCIE;
    NVIC_EnableIRQ(ADC_IRQn);
    return true;
}

void bsp_adc_stop(void)
{
    ADC1->CR1 &= ~ADC_CR1_JEOCIE;
    ADC1->CR2 &= ~ADC_CR2_ADON;
    ADC2->CR2 &= ~ADC_CR2_ADON;
}

bool bsp_adc_read(void)
{
    if ((ADC1->SR & ADC_SR_JEOC) == 0u || (ADC2->SR & ADC_SR_JEOC) == 0u) {
        return false;
    }
    adc_m1.b_voltage = (float)ADC1->JDR1 * (3.3f / 4095.0f);
    adc_m1.c_voltage = (float)ADC2->JDR1 * (3.3f / 4095.0f);
    adc_bus_voltage = (float)ADC1->JDR2 * ((3.3f / 4095.0f) * (41.2f / 2.2f));
    ADC1->SR = ~(ADC_SR_JEOC | ADC_SR_JSTRT);
    ADC2->SR = ~(ADC_SR_JEOC | ADC_SR_JSTRT);
    return true;
}
