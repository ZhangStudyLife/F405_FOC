#ifndef APP_HARDWARE_BSP_BSP_ADC_H
#define APP_HARDWARE_BSP_BSP_ADC_H

#include <stdbool.h>

typedef struct {
    float b_voltage; /* V: PC3 / ADC1_IN13 / M1_SO1, B-phase shunt amplifier. */
    float c_voltage; /* V: PC2 / ADC2_IN12 / M1_SO2, C-phase shunt amplifier. */
} bsp_adc_m1_t;

/* ADC pin voltages including bias, using nominal VDDA=3.3 V; no current conversion.
   Updated together in the ADC ISR at 20 kHz. Foreground coherent reads must
   briefly mask the ADC IRQ; volatile alone does not make the pair atomic. */
extern volatile bsp_adc_m1_t adc_m1;
/* V: DCBUS via PA6 / VBUS_S, 39k / 2.2k divider; updated at 20 kHz. */
extern volatile float adc_bus_voltage;

/* Call after MX_ADC1/2_Init with TIM8 stopped. */
bool bsp_adc_start(void);
void bsp_adc_stop(void);
/* ADC ISR only: publish all voltages, or return false without updating any. */
bool bsp_adc_read(void);

#endif
