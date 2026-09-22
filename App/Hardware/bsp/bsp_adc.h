#ifndef APP_HARDWARE_BSP_ADC_H
#define APP_HARDWARE_BSP_ADC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float b_voltage, c_voltage, bus_voltage;
} bsp_adc_sample_t;

/* 20 kHz; nominal VDDA=3.3 V, shunt bias retained, no current calibration.
   Foreground code must briefly disable IRQs for a coherent snapshot. */
extern volatile bsp_adc_sample_t adc_sample;
/* Raw 12-bit codes behind adc_sample: B/C phase, then bus. Logged so a host can
   re-derive currents once a reference measurement exists. */
extern volatile uint16_t adc_raw_b, adc_raw_c, adc_raw_bus;
extern volatile uint32_t adc_errors;
#ifdef FOC_CAPTURE
extern volatile uint16_t adc_debug[4]; /* B/C/bus raw codes and ADC-read entry CNT, not hold time. */
#endif

void bsp_adc_start(void);
void bsp_adc_stop(void);
bool bsp_adc_read(void); /* SPI RX completion after both ADC ranks, or ADC/DMA error IRQ. */

#endif
