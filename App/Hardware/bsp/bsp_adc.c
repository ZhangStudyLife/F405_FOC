#include "bsp_adc.h"
#include "stm32f4xx_hal.h"
#include <math.h>

static volatile uint32_t s_raw[2]; /* CDR: ADC2 in high half, ADC1 in low half. */
volatile bsp_adc_sample_t adc_sample;
volatile uint32_t adc_errors;

void bsp_adc_start(void)
{
    /* Sampling only: disconnect all six gate pins and keep them low. */
    GPIO_InitTypeDef gpio = {.Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL};
    GPIOA->BSRR = GPIO_PIN_7 << 16;
    GPIOB->BSRR = (GPIO_PIN_0 | GPIO_PIN_1) << 16;
    GPIOC->BSRR = (GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8) << 16;
    gpio.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8;
    HAL_GPIO_Init(GPIOC, &gpio);
    TIM8->BDTR &= ~TIM_BDTR_MOE;
    TIM8->CCER = TIM_CCER_CC4E;
    TIM8->CR2 = TIM_TRGO_OC4REF;
    TIM8->PSC = 0u; /* 168 MHz / (2 * 4200) = 20 kHz. */
    TIM8->CNT = 0u;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0u;
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
    DMA2->LIFCR = 0x3du;
    DMA2_Stream0->PAR = (uint32_t)&ADC->CDR;
    DMA2_Stream0->M0AR = (uint32_t)s_raw;
    DMA2_Stream0->NDTR = 2u;
    DMA2_Stream0->CR = DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_PL_1 |
                      DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 |
                      DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE | DMA_SxCR_EN;
    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0u, 0u);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    ADC2->CR2 = ADC_CR2_ADON;
    ADC1->CR2 = ADC_EXTERNALTRIGCONV_T8_TRGO | ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_CR2_ADON;
    HAL_Delay(1u); /* ADC stabilization; only at startup. */
    TIM8->BDTR |= TIM_BDTR_MOE; /* CH4 only; no gate output. */
    TIM8->CR1 |= TIM_CR1_CEN;
}

bool bsp_adc_read(void)
{
    uint32_t flags = DMA2->LISR;
    DMA2->LIFCR = 0x3du;
    if ((flags & (DMA_LISR_TCIF0 | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0)) != DMA_LISR_TCIF0 ||
        ((ADC1->SR | ADC2->SR) & ADC_SR_OVR)) {
        TIM8->CR1 &= ~TIM_CR1_CEN;
        ADC1->CR1 = ADC2->CR1 = 0u;
        ADC1->CR2 = ADC2->CR2 = 0u;
        adc_sample.b_voltage = adc_sample.c_voltage = adc_sample.bus_voltage = NAN;
        adc_errors++;
        return false;
    }
    uint32_t phases = s_raw[0];
    adc_sample.b_voltage = (float)(phases & 0xffffu) * (3.3f / 4095.0f);
    adc_sample.c_voltage = (float)(phases >> 16) * (3.3f / 4095.0f);
    adc_sample.bus_voltage = (float)(s_raw[1] & 0xffffu) * ((3.3f / 4095.0f) * (41.2f / 2.2f));
    return true;
}
