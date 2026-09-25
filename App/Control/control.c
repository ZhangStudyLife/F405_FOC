#include "control.h"
#include "foc.h"
#include <math.h>

/* Basic cascade: position P -> speed PI -> Iq_ref, on top of the unchanged
   20 kHz current loop. Output limits match the current-loop command range, so
   the outer loop can never ask for more than a manual `Iq` command could.
   Tuning order: speed Kp first until it tracks without oscillating, then speed
   Ki to remove the steady-state error, then position Kp. */
#define CONTROL_BANDWIDTH 0.1f  /* Integrator back-calculation gain. */
#define CONTROL_PERIOD_US 1000u /* Outer-loop period; runs once per millisecond. */
#define CONTROL_JUMP_US 4000u   /* Gap above this is a discontinuity, not a dt. */
static uint32_t mode, fault;
static float position, reference, speed, speed_target, position_target;
static float motion_speed = 100.0f, motion_accel = 1000.0f, motion_jerk = 10000.0f;
static float profile_speed, profile_accel;
static float integral_speed, last_deg;
static uint32_t previous_tick;
static bool tracking, commanded;

float control_iq_ref(void) { return reference; }
uint32_t control_mode(void) { return mode; }
uint32_t control_fault(void) { return fault; }
float control_speed_rpm(void) { return speed; }
float control_speed_target(void) { return speed_target; }
float control_position_deg(void) { return position; }
float control_position_target(void) { return position_target; }

bool control_scheduled(void)
{
    return commanded;
}

void control_stop(void)
{
    mode = CONTROL_TORQUE;
    fault = 0u;
    commanded = false;
    reference = speed_target = position_target = 0.0f;
    profile_speed = profile_accel = 0.0f;
    integral_speed = 0.0f;
}

static void accept(void)
{
    commanded = true;
    fault = 0u;
}

/* Shared enable test. Idle with a zero torque target must not start, matching
   the existing `Iq 0` semantics; speed and position mode always start, because
   holding zero speed or holding the present position is a real request to run.
   `stop` remains the only way down. */
static bool arm(uint32_t wanted, float target)
{
    if (wanted != CONTROL_TORQUE) return true;
    return foc.state != FOC_IDLE || target != 0.0f;
}

/* Start from idle through the existing PRECHARGE interlock, whose two
   milliseconds of all-low-side conduction are also what foc_calibrate uses.
   The target is already set by the caller; foc_stop() cleared the previous one. */
static bool go(void)
{
    if (foc.state != FOC_IDLE) return true;
    foc.state = FOC_PRECHARGE;
    return true;
}

/* Repeating a target in the same mode must not reset the integrator. */
static bool continuous(uint32_t wanted)
{
    return mode == wanted && commanded;
}

static bool start(uint32_t wanted, float target)
{
    if (!arm(wanted, target)) return false;
    if (!foc.calibrated || !foc.zero_ready) return false;
    if (foc.state != FOC_IDLE && foc.state != FOC_RUN && foc.state != FOC_PRECHARGE) return false;
    if (!continuous(wanted)) integral_speed = 0.0f;
    if (!go()) return false;
    mode = wanted;
    accept();
    return true;
}

bool control_torque(float amps)
{
    if (!isfinite(amps) || fabsf(amps) > FOC_CURRENT_MAX) return false;
    foc.command = amps; /* The ramp owns iq_ref in torque mode. */
    if (!start(CONTROL_TORQUE, amps)) return false;
    speed_target = 0.0f;
    return true;
}

bool control_speed(float rpm)
{
    if (!start(CONTROL_SPEED, rpm)) return false;
    speed_target = rpm;
    return true;
}

bool control_position(float deg)
{
    bool was_position = continuous(CONTROL_POSITION);
    if (!start(CONTROL_POSITION, deg)) return false;
    if (!was_position) { profile_speed = speed; profile_accel = 0.0f; }
    position_target = deg;
    return true;
}

bool control_motion(float rpm, float acceleration, float jerk)
{
    if (!isfinite(rpm) || !isfinite(acceleration) || !isfinite(jerk) ||
        rpm <= 0.0f || rpm > FOC_SPEED_MAX || acceleration <= 0.0f ||
        acceleration > 100000.0f || jerk <= 0.0f || jerk > 1000000.0f) return false;
    motion_speed = rpm;
    motion_accel = acceleration;
    motion_jerk = jerk;
    return true;
}

bool control_zero(void)
{
    if (foc.state != FOC_IDLE || fabsf(foc.rpm) >= 5.0f) return false;
    position = 0.0f;
    profile_speed = profile_accel = 0.0f;
    tracking = false;
    return true;
}

/* Jerk-limited position velocity, replanned from the remaining distance each
   millisecond. Its speed and acceleration survive mid-move target changes. */
static void position_profile(float dt)
{
    float error = position_target - position;
    float direction = error > 0.0f ? 1.0f : error < 0.0f ? -1.0f : 0.0f;
    float desired = fminf(motion_speed, MOTOR_POSITION_KP * fabsf(error)) * direction;
    float v = fmaxf(fabsf(profile_speed), fabsf(speed)) * 6.0f;
    float a = motion_accel * 6.0f, j = motion_jerk * 6.0f;
    float stop = v * v / (2.0f * a) + v * a / (2.0f * j);
    if (profile_speed * direction > 0.0f) {
        stop += v * fmaxf(0.0f, profile_accel * direction * 6.0f) / j;
        if (fabsf(error) <= stop * 2.0f) desired = 0.0f;
    }
    float requested = fmaxf(-motion_accel, fminf(motion_accel, (desired - profile_speed) / dt));
    if (profile_accel > 0.0f && desired > profile_speed &&
        profile_speed + profile_accel * profile_accel / (2.0f * motion_jerk) >= desired) requested = 0.0f;
    if (profile_accel < 0.0f && desired < profile_speed &&
        profile_speed - profile_accel * profile_accel / (2.0f * motion_jerk) <= desired) requested = 0.0f;
    float change = fmaxf(-motion_jerk * dt, fminf(motion_jerk * dt, requested - profile_accel));
    profile_accel += change;
    profile_speed += profile_accel * dt;
    speed_target = profile_speed;
}

/* Speed PI with the position profile above it. foc.rpm is encoder-filtered. */
static float outer_output(float dt)
{
    if (mode == CONTROL_POSITION) position_profile(dt);
    float error = speed_target - speed;
    float wanted = MOTOR_SPEED_KP * error + integral_speed;
    float limited = wanted;
    if (limited > FOC_CURRENT_MAX) limited = FOC_CURRENT_MAX;
    if (limited < -FOC_CURRENT_MAX) limited = -FOC_CURRENT_MAX;
    integral_speed += MOTOR_SPEED_KI * error * dt;
    if (limited != wanted) integral_speed += CONTROL_BANDWIDTH * (limited - wanted);
    if (integral_speed > FOC_CURRENT_MAX) integral_speed = FOC_CURRENT_MAX;
    if (integral_speed < -FOC_CURRENT_MAX) integral_speed = -FOC_CURRENT_MAX;
    return limited;
}

void control_step(uint32_t sample_us, float mechanical_deg)
{
    if (foc.fault && !fault) fault = foc.fault; /* A trip latches this loop too. */
    if (isnan(mechanical_deg)) return;
    uint32_t elapsed = (sample_us - previous_tick) & 0xffffffu;
    if (elapsed < CONTROL_PERIOD_US) return;
    /* At the 8600 RPM limit, one millisecond moves less than 52 degrees. */
    if (!tracking) { last_deg = mechanical_deg; tracking = true; }
    float step_deg = mechanical_deg - last_deg; /* foc_step already unwraps. */
    last_deg = mechanical_deg;
    position += step_deg;
    if (elapsed > CONTROL_JUMP_US) {
        previous_tick = sample_us; /* Resynchronise without integrating the gap. */
        return;
    }
    float dt = (float)elapsed * 1e-6f;
    previous_tick = sample_us;
    speed = foc.rpm;
    if (!commanded || mode == CONTROL_TORQUE) return; /* Torque keeps its own Iq reference. */
    reference = outer_output(dt);
}
