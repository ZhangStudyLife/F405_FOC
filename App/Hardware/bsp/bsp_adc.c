#include "bsp_adc.h"
#include "stm32f4xx_hal.h"
#include "bsp_time.h"

bool bsp_adc_config_valid(void)
{
    /* Two ranks: JSQ3 is rank 1, JSQ4 is rank 2. */
    if (ADC1->JSQR != (ADC_JSQR_JL_0 | (13u << 10) | (4u << 15)) ||
        ADC2->JSQR != (ADC_JSQR_JL_0 | (12u << 10) | (6u << 15)) ||
        (ADC->CCR & (ADC_CCR_MULTI | ADC_CCR_ADCPRE)) !=
            (ADC_DUALMODE_INJECSIMULT | ADC_CLOCK_SYNC_PCLK_DIV4) ||
        ((ADC1->CR2 | ADC2->CR2) & ADC_CR2_ALIGN) != 0u ||
        (ADC1->CR1 & ADC2->CR1 & ADC_CR1_SCAN) == 0u ||
        ((ADC1->SMPR1 >> 9) & 7u) != ADC_SAMPLETIME_28CYCLES ||
        ((ADC2->SMPR1 >> 6) & 7u) != ADC_SAMPLETIME_28CYCLES ||
        ((ADC1->SMPR2 >> 12) & 7u) != ADC_SAMPLETIME_15CYCLES ||
        ((ADC2->SMPR2 >> 18) & 7u) != ADC_SAMPLETIME_15CYCLES ||
        (ADC1->CR2 & (ADC_CR2_JEXTSEL | ADC_CR2_JEXTEN)) !=
            (ADC_EXTERNALTRIGINJECCONV_T8_CC4 | ADC_EXTERNALTRIGINJECCONVEDGE_RISING) ||
        (ADC2->CR2 & ADC_CR2_JEXTEN) != 0u ||
        ((ADC1->CR1 | ADC2->CR1) & (ADC_CR1_RES | ADC_CR1_JAUTO | ADC_CR1_JDISCEN)) != 0u) {
        return false;
    }

    return true;
}

bool bsp_adc_start(void)
{
    if (!bsp_adc_config_valid()) return false;
    ADC1->SR = 0u;
    ADC2->SR = 0u;
    ADC2->CR1 &= ~ADC_CR1_JEOCIE;
    ADC2->CR2 |= ADC_CR2_ADON;
    ADC1->CR2 |= ADC_CR2_ADON;
    bsp_time_delay_us(3u);
    NVIC_ClearPendingIRQ(ADC_IRQn);
    ADC1->CR1 |= ADC_CR1_JEOCIE;
    NVIC_EnableIRQ(ADC_IRQn);
    return true;
}

void bsp_adc_stop(void)
{
    ADC1->CR1 &= ~ADC_CR1_JEOCIE;
    ADC2->CR1 &= ~ADC_CR1_JEOCIE;
    ADC1->CR2 &= ~ADC_CR2_ADON;
    ADC2->CR2 &= ~ADC_CR2_ADON;
}

bool bsp_adc_read(bsp_adc_frame_t *frame)
{
    if ((ADC1->SR & ADC_SR_JEOC) == 0u || (ADC2->SR & ADC_SR_JEOC) == 0u) {
        return false;
    }
    frame->m[0] = (uint16_t)ADC1->JDR1;
    frame->s[0] = (uint16_t)ADC2->JDR1;
    frame->m[1] = (uint16_t)ADC1->JDR2;
    frame->s[1] = (uint16_t)ADC2->JDR2;
    ADC1->SR = ~(ADC_SR_JEOC | ADC_SR_JSTRT);
    ADC2->SR = ~(ADC_SR_JEOC | ADC_SR_JSTRT);
    return true;
}
