#ifndef APP_CONTROL_CONTROL_H
#define APP_CONTROL_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
#include "motor_params.h"
enum { CONTROL_TORQUE, CONTROL_SPEED, CONTROL_POSITION };
typedef struct {
    uint32_t mode;
    bool active;
    float iq_ref, speed, speed_target, position, position_target;
} control_t;
extern control_t control; /* ISR-owned; foreground commands require IRQ lock. */
bool control_command(uint32_t mode, float target);
bool control_zero(void);
void control_stop(void);
/* 20 kHz protected angle/observer update; PI and position run at 1 kHz. */
void control_step(uint32_t sample_us, float mechanical_deg, float wrapped_deg);
#endif
