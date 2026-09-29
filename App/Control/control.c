#include "control.h"
#include "foc.h"
#include <math.h>

motor_params_t motor_params = MOTOR_PARAMS_DEFAULT;
control_t control;
static float integral, last_deg;
static uint32_t previous_tick;
static bool tracking;

void control_stop(void)
{
    control.mode = CONTROL_TORQUE;
    control.active = false;
    control.iq_ref = control.speed_target = control.position_target = integral = 0;
}

bool control_command(uint32_t mode, float target)
{
    if (mode > CONTROL_POSITION || !isfinite(target)) return false;
    float limit = mode == CONTROL_TORQUE ? FOC_CURRENT_MAX : mode == CONTROL_SPEED ? FOC_SPEED_MAX : 1e6f;
    if (fabsf(target) > limit || !foc.calibrated || !foc.zero_ready) return false;
    if (foc.state != FOC_IDLE && foc.state != FOC_RUN && foc.state != FOC_PRECHARGE) return false;
    if (foc.state == FOC_IDLE && mode == CONTROL_TORQUE && target == 0) return true;
    if (mode != control.mode || !control.active) {
        integral = control.iq_ref = 0;
        control.speed_target = 0;
    }
    if (foc.state == FOC_IDLE && !foc_current(mode == CONTROL_TORQUE ? target : 0.01f)) return false;
    foc.command = mode == CONTROL_TORQUE ? target : 0;
    control.mode = mode;
    control.active = true;
    if (mode == CONTROL_SPEED) control.speed_target = target;
    if (mode == CONTROL_POSITION) control.position_target = target;
    return true;
}

bool control_zero(void)
{
    if (foc.state != FOC_IDLE || fabsf(foc.rpm) >= 5) return false;
    control.position = 0;
    tracking = false;
    return true;
}

void control_step(uint32_t sample_us, float mechanical_deg)
{
    if (!isfinite(mechanical_deg)) return;
    uint32_t elapsed = (sample_us - previous_tick) & 0xffffffu;
    if (elapsed < 1000u) return;
    if (!tracking) { last_deg = mechanical_deg; tracking = true; }
    control.position += mechanical_deg - last_deg;
    last_deg = mechanical_deg;
    previous_tick = sample_us;
    control.speed = foc.rpm;
    if (elapsed > 4000u || foc.state != FOC_RUN || !control.active || control.mode == CONTROL_TORQUE) return;
    if (control.mode == CONTROL_POSITION)
        control.speed_target = fmaxf(-motor_params.position_speed, fminf(motor_params.position_speed,
            motor_params.position_kp * (control.position_target - control.position)));
    float error = control.speed_target - control.speed;
    float wanted = motor_params.speed_kp * error + integral;
    float limited = fmaxf(-FOC_CURRENT_MAX, fminf(FOC_CURRENT_MAX, wanted));
    /* Speed/position use encoder-positive coordinates; Iq uses phase order. */
    control.iq_ref = (float)foc.calibration.direction * limited;
    integral += motor_params.speed_ki * error * ((float)elapsed * 1e-6f);
    integral += 0.1f * (limited - wanted);
    integral = fmaxf(-FOC_CURRENT_MAX, fminf(FOC_CURRENT_MAX, integral));
}
