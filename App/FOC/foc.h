#ifndef APP_FOC_H
#define APP_FOC_H

#include <stdbool.h>
#include <stdint.h>

enum { FOC_IDLE, FOC_PRECHARGE, FOC_CALIBRATE, FOC_SAVE, FOC_RUN, FOC_FAULT };
enum { FOC_OK, FOC_SENSOR, FOC_ADC, FOC_TIMING, FOC_WINDOW, FOC_ALIGNMENT, FOC_FLASH, FOC_UART };
typedef struct { float zero; int32_t direction; } foc_calibration_t;
typedef struct {
    foc_calibration_t calibration;
    float duty[3], uq, rpm;
    volatile uint32_t state, fault;
    bool calibrated;
} foc_t;
extern foc_t foc; /* ISR-owned; foreground changes require a short IRQ critical section. */

void foc_init(const foc_calibration_t *calibration);
bool foc_run(float volts);
bool foc_calibrate(void);
void foc_stop(void);
void foc_trip(uint32_t fault);
void foc_step(float mechanical_deg); /* Exactly 20 kHz. */
float foc_wrap(float radians);
void foc_modulate(float theta, float ud, float uq, float duty[3]);
bool foc_window(const float duty[3]);

#endif
