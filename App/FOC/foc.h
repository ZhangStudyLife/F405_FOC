#ifndef APP_FOC_H
#define APP_FOC_H

#include <stdbool.h>
#include <stdint.h>

#define FOC_BUS_MIN 8.0f
#define FOC_BUS_MAX 36.0f
#define FOC_SPEED_MAX 6000.0f /* Software overspeed trip, RPM. */

/* Nominal timing, pending scope validation of ADC/driver/analog delays. */
#ifndef FOC_TRIGGER_TICKS
#define FOC_TRIGGER_TICKS 4100u
#endif
#define FOC_APERTURE_TICKS 224u
#define FOC_TRIGGER_ALLOWANCE 24u
#define FOC_SETTLE_TICKS 588u /* 500 ns dead time + 3 us analog settling. */
#define FOC_HOLD_TICKS (FOC_TRIGGER_TICKS + FOC_APERTURE_TICKS + FOC_TRIGGER_ALLOWANCE)
#define FOC_EDGE_LIMIT ((FOC_TRIGGER_TICKS < 8400u - FOC_HOLD_TICKS ? \
                        FOC_TRIGGER_TICKS : 8400u - FOC_HOLD_TICKS) - FOC_SETTLE_TICKS - 2u)

enum { FOC_IDLE, FOC_PRECHARGE, FOC_CALIBRATE, FOC_SAVE, FOC_RUN, FOC_FAULT, FOC_OFFSET };
enum { FOC_OK, FOC_SENSOR, FOC_ADC, FOC_TIMING, FOC_WINDOW, FOC_ALIGNMENT,
       FOC_FLASH, FOC_UART, FOC_BUS, FOC_ZERO, FOC_CURRENT, FOC_SPEED };
typedef struct { float zero; int32_t direction; } foc_calibration_t;
typedef struct {
    foc_calibration_t calibration;
    float duty[3], ud, uq, command, iq_ref, id, iq, electrical_deg, rpm;
    float b_offset, c_offset;
    volatile uint32_t state, fault;
    bool calibrated, zero_ready;
} foc_t;
extern foc_t foc; /* ISR-owned; foreground changes require a short IRQ critical section. */

void foc_init(const foc_calibration_t *calibration);
bool foc_current(float amps); /* +/-0.8 A torque, no reference ramp; zero does not start. */
bool foc_calibrate(void);
void foc_stop(void);
void foc_trip(uint32_t fault);
/* Exactly 20 kHz. delay is SPI CS time minus nominal ADC hold end, seconds.
   MT6835 internal measurement delay is not calibrated. */
void foc_step(float mechanical_deg, float bus_voltage, float b_voltage, float c_voltage, float encoder_delay);
float foc_wrap(float radians);
/* Returns applied vector scale for PI anti-windup; shifts common mode for ADC. */
float foc_modulate(float alpha, float beta, float bus_voltage, float duty[3]);
bool foc_window(const float duty[3]);

#endif
