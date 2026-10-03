#include "control.h"
#include "foc.h"
#include "mt6835_port_stm32.h"
#include <math.h>

motor_params_t motor_params = MOTOR_PARAMS_DEFAULT;
control_t control;
static const float cogging[512] = {
#include "cogging_table.inc"
};
static float integral, last_p, last_deg, phase_error;
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
        last_p = -control.speed;
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
    if (!isfinite(mechanical_deg) || !isfinite(mt6835_raw_deg) || !isfinite(foc.angle_step)) return;
    if (!tracking) {
        last_deg = mechanical_deg;
        phase_error = 0;
        control.speed = foc.rpm;
        tracking = true;
    }
    /* Critically damped encoder PLL: kp=2*w, ki=w*w. Keep its angle error
       relative to the protected encoder increment to avoid float roundoff
       at large absolute angles. Neither target nor current predicts speed. */
    phase_error += foc.angle_step - 0.0003f * control.speed;
    control.speed += MOTOR_SPEED_PLL_RAD_S * MOTOR_SPEED_PLL_RAD_S * (5e-5f / 6.0f) * phase_error;
    phase_error *= 1.0f - 2.0f * MOTOR_SPEED_PLL_RAD_S * 5e-5f;
    uint32_t elapsed = (sample_us - previous_tick) & 0xffffffu;
    if (elapsed < 1000u) return;
    control.position += mechanical_deg - last_deg;
    last_deg = mechanical_deg;
    previous_tick = sample_us;
    if (elapsed > 4000u || foc.state != FOC_RUN || !control.active || control.mode == CONTROL_TORQUE) return;
    if (control.mode == CONTROL_POSITION)
        control.speed_target = fmaxf(-motor_params.position_speed, fminf(motor_params.position_speed,
            motor_params.position_kp * (control.position_target - control.position)));
    float error = control.speed_target - control.speed;
    float p = MOTOR_SPEED_REFERENCE_WEIGHT * control.speed_target - control.speed;
    float dt = (float)elapsed * 1e-6f;
    float wanted = integral + motor_params.speed_kp * (p - last_p) + motor_params.speed_ki * error * dt;
    last_p = p;
    float x = mt6835_raw_deg * (512.0f / 360.0f);
    unsigned i = (unsigned)x & 511u;
    float fraction = x - (float)i;
    float fade = fmaxf(0.0f, fminf(1.0f, (MOTOR_COGGING_OFF_RPM - fabsf(control.speed)) /
        (MOTOR_COGGING_OFF_RPM - MOTOR_COGGING_FULL_RPM)));
    float feedforward = fade * (cogging[i] + fraction * (cogging[(i + 1u) & 511u] - cogging[i]));
    float total = wanted + feedforward;
    float limited = fmaxf(-FOC_CURRENT_MAX, fminf(FOC_CURRENT_MAX, total));
    float previous = (float)foc.calibration.direction * control.iq_ref;
    float step = MOTOR_SPEED_IQ_SLEW_A_S * dt;
    limited = fmaxf(previous - step, fminf(previous + step, limited));
    /* Track the final amplitude/rate limit, including feedforward. Incremental PI avoids
       a beta-dependent DC offset being clipped as an integral current. */
    float tracking_gain = motor_params.speed_kp > 0.0f ? motor_params.speed_ki / motor_params.speed_kp * dt : 1.0f;
    integral = wanted + fminf(1.0f, tracking_gain) * (limited - total);
    control.iq_ref = (float)foc.calibration.direction * limited;
}
