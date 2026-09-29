#ifndef APP_MOTOR_PARAMS_H
#define APP_MOTOR_PARAMS_H
#include "motor_config.h"
#include <stdbool.h>
#include <math.h>
typedef struct {
    float current_kp, current_ki, speed_kp, speed_ki, position_kp;
    float current_ramp, position_speed;
} motor_params_t;
#define MOTOR_PARAMS_DEFAULT {MOTOR_CURRENT_KP, MOTOR_CURRENT_KI_TS * 20000.0f, MOTOR_SPEED_KP, MOTOR_SPEED_KI, MOTOR_POSITION_KP, 10.0f, 100.0f}
extern motor_params_t motor_params;
static inline bool motor_params_valid(const motor_params_t *p)
{
    return isfinite(p->current_kp) && p->current_kp >= 0 && p->current_kp <= 10 &&
        isfinite(p->current_ki) && p->current_ki >= 0 && p->current_ki <= 20000 &&
        isfinite(p->speed_kp) && p->speed_kp >= 0 && p->speed_kp <= 1 &&
        isfinite(p->speed_ki) && p->speed_ki >= 0 && p->speed_ki <= 10 &&
        isfinite(p->position_kp) && p->position_kp >= 0 && p->position_kp <= 100 &&
        isfinite(p->current_ramp) && p->current_ramp > 0 && p->current_ramp <= 1000 &&
        isfinite(p->position_speed) && p->position_speed > 0 && p->position_speed <= MOTOR_SPEED_MAX_RPM;
}
#endif
