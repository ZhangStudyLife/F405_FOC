#ifndef APP_CONTROL_CONTROL_H
#define APP_CONTROL_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* Outer loops below the 20 kHz current loop. Only the 1 kHz ISR path
   (control_step) owns their state; commands only publish targets. */
enum { CONTROL_TORQUE, CONTROL_SPEED, CONTROL_POSITION };

bool control_torque(float amps);      /* Target Iq, amps. */
bool control_speed(float rpm);        /* Target speed, RPM, signed. */
bool control_position(float deg);     /* Target position, mechanical degrees, multi-turn. */
bool control_hold_position(void);     /* Re-target the last position, still scheduled. */
bool control_zero(void);              /* Redefine the current position as 0 deg. */
void control_stop(void);              /* stop/trip: torque mode, cleared integrators. */

/* 20 kHz. Accumulates multi-turn mechanical position every call and runs the
   outer loop once per millisecond, timed from the free-running sample counter. */
void control_step(uint32_t sample_us, float mechanical_deg);

/* Latched reason to trip the FOC state machine; read once per 20 kHz cycle. */
uint32_t control_fault(void);
bool control_scheduled(void);         /* Target was received recently enough to keep running. */
uint32_t control_mode(void);          /* enum above; 0 means the current loop holds iq_ref. */
float control_iq_ref(void);           /* Speed/position output, amps. */
float control_speed_rpm(void);        /* Measured, from position difference over 1 ms. */
float control_speed_target(void);
float control_position_deg(void);     /* Multi-turn, relative to the last control_zero(). */
float control_position_target(void);

#endif
