#ifndef APP_HARDWARE_BSP_BSP_ADC_H
#define APP_HARDWARE_BSP_BSP_ADC_H

#include <stdbool.h>
#include <stdint.h>

/* Fixed board sequence: ADC1 PC3/PA4, ADC2 PC2/PA6. Raw 12-bit codes. */
typedef struct {
    uint16_t m[2];
    uint16_t s[2];
} bsp_adc_frame_t;

/* Call after MX_ADC1/2_Init, with the trigger timer stopped. */
bool bsp_adc_config_valid(void);
bool bsp_adc_start(void);
void bsp_adc_stop(void);
/* ADC IRQ only. Returns false unless BOTH injected sequences completed.
   Reads the four results and acknowledges only injected status flags. */
bool bsp_adc_read(bsp_adc_frame_t *frame);

#endif
