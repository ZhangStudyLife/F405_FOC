#include "bsp_adc.h"
#include "bsp_motor.h"
#include "stm32f4xx_hal.h"
#include <math.h>

static volatile uint32_t s_raw[2]; /* CDR: ADC2 in high half, ADC1 in low half. */
volatile bsp_adc_sample_t adc_sample;
volatile uint32_t adc_errors;
#ifdef FOC_CAPTURE
volatile uint16_t adc_debug[4];
#endif

void bsp_adc_start(void)
{
    bsp_motor_init();
    TIM8->CCER = TIM_CCER_CC4E;
    TIM8->CR2 = TIM_TRGO_OC4REF;
    DBGMCU->APB2FZ |= DBGMCU_APB2_FZ_DBG_TIM8_STOP;

    ADC1->CR2 = ADC2->CR2 = 0u;
    ADC->CCR = ADC_DUALMODE_REGSIMULT | ADC_CLOCK_SYNC_PCLK_DIV4 |
               ADC_DMAACCESSMODE_2 | ADC_CCR_DDS;
    ADC1->CR1 = ADC2->CR1 = ADC_CR1_SCAN | ADC_CR1_OVRIE;
    ADC1->SQR1 = ADC2->SQR1 = ADC_SQR1_L_0;
    ADC1->SQR3 = 13u | (6u << 5);
    ADC2->SQR3 = 12u | (6u << 5);
    ADC1->SMPR1 = ADC_SAMPLETIME_28CYCLES << 9;
    ADC2->SMPR1 = ADC_SAMPLETIME_28CYCLES << 6;
    ADC1->SMPR2 = ADC2->SMPR2 = ADC_SAMPLETIME_15CYCLES << 18;
    ADC1->SR = ADC2->SR = 0u;

    __HAL_RCC_DMA2_CLK_ENABLE();
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    while (DMA2_Stream0->CR & DMA_SxCR_EN) {}
    DMA2->LIFCR = 0x3du;
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
    DMA2_Stream0->PAR = (uint32_t)&ADC->CDR;
    DMA2_Stream0->M0AR = (uint32_t)s_raw;
    DMA2_Stream0->NDTR = 2u;
    DMA2_Stream0->CR = DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_PL_1 |
                      DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 |
                      DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE | DMA_SxCR_EN;
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    ADC2->CR2 = ADC_CR2_ADON;
    ADC1->CR2 = ADC_EXTERNALTRIGCONV_T8_TRGO | ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_CR2_ADON;
    HAL_Delay(1u); /* ADC stabilization; only at startup. */
    TIM8->BDTR |= TIM_BDTR_MOE; /* CH4 only; no gate output. */
    TIM8->CR1 |= TIM_CR1_CEN;
}

bool bsp_adc_read(void)
{
#ifdef FOC_CAPTURE
    adc_debug[3] = (uint16_t)TIM8->CNT;
#endif
    uint32_t flags = DMA2->LISR;
    DMA2->LIFCR = 0x3du;
    if ((flags & (DMA_LISR_TCIF0 | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0)) != DMA_LISR_TCIF0 ||
        ((ADC1->SR | ADC2->SR) & ADC_SR_OVR)) {
        bsp_motor_off();
        TIM8->DIER = 0u;
        TIM8->CR1 &= ~TIM_CR1_CEN;
        ADC1->CR1 = ADC2->CR1 = 0u;
        ADC1->CR2 = ADC2->CR2 = 0u;
        adc_sample.b_voltage = adc_sample.c_voltage = adc_sample.bus_voltage = NAN;
        adc_errors++;
        return false;
    }
    uint32_t phases = s_raw[0];
#ifdef FOC_CAPTURE
    adc_debug[0] = (uint16_t)phases;
    adc_debug[1] = (uint16_t)(phases >> 16);
    adc_debug[2] = (uint16_t)s_raw[1];
#endif
    adc_sample.b_voltage = (float)(phases & 0xffffu) * (3.3f / 4095.0f);
    adc_sample.c_voltage = (float)(phases >> 16) * (3.3f / 4095.0f);
    adc_sample.bus_voltage = (float)(s_raw[1] & 0xffffu) * ((3.3f / 4095.0f) * (41.2f / 2.2f));
    return true;
}

void bsp_adc_stop(void)
{
    bsp_motor_off();
    TIM8->DIER = 0u;
    TIM8->CR1 &= ~TIM_CR1_CEN;
    ADC1->CR1 = ADC2->CR1 = 0u;
    ADC1->CR2 = ADC2->CR2 = 0u;
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    while (DMA2_Stream0->CR & DMA_SxCR_EN) {}
    DMA2->LIFCR = 0x3du;
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    HAL_NVIC_ClearPendingIRQ(ADC_IRQn);
    HAL_NVIC_ClearPendingIRQ(TIM8_UP_TIM13_IRQn);
}
