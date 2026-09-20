#ifndef APP_HARDWARE_BSP_ADC_H
#define APP_HARDWARE_BSP_ADC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float b_voltage, c_voltage, bus_voltage;
} bsp_adc_sample_t;

/* 20 kHz; nominal VDDA=3.3 V, shunt bias retained, no current calibration.
   Foreground code must mask DMA2_Stream0 IRQ to read a coherent snapshot. */
extern volatile bsp_adc_sample_t adc_sample;
extern volatile uint32_t adc_errors;

void bsp_adc_start(void);
bool bsp_adc_read(void); /* DMA2 stream 0 / ADC overrun IRQ only. */

#endif
