#include "control.h"
#include "bsp_uart.h"
#include "foc.h"
#include <math.h>

/* Basic cascade: position P -> speed PI -> Iq_ref, on top of the unchanged
   20 kHz current loop. Output limits match the current-loop command range, so
   the outer loop can never ask for more than a manual `Iq` command could.
   Tuning order: speed Kp first until it tracks without oscillating, then speed
   Ki to remove the steady-state error, then position Kp. */
#define SPEED_KP 0.005f         /* A/RPM. 100 RPM error -> 0.5 A. */
#define SPEED_KI 0.01f          /* A/(RPM*s). */
#define POSITION_KP 4.0f       /* RPM per degree. */
#define CONTROL_BANDWIDTH 0.1f  /* Integrator back-calculation gain. */
#define CONTROL_PERIOD_US 1000u /* Outer-loop period; runs once per millisecond. */
#define CONTROL_JUMP_US 4000u   /* Gap above this is a discontinuity, not a dt. */
#define CONTROL_COMMAND_MS 200u /* Stop if the host stops sending mode targets. */
static uint32_t mode, fault;
static float position, reference, speed, speed_target, position_target;
static float integral_speed, last_deg;
static uint32_t previous_tick, command_ms;
static bool tracking, commanded;

float control_iq_ref(void) { return reference; }
uint32_t control_mode(void) { return mode; }
uint32_t control_fault(void) { return fault; }
float control_speed_rpm(void) { return speed; }
float control_speed_target(void) { return speed_target; }
float control_position_deg(void) { return position; }
float control_position_target(void) { return position_target; }

/* A held output demands a live host; used only while commanded. */
bool control_scheduled(void)
{
    return commanded && (uint32_t)(bsp_uart_millis() - command_ms) < CONTROL_COMMAND_MS;
}

void control_stop(void)
{
    mode = CONTROL_TORQUE;
    fault = 0u;
    commanded = false;
    reference = speed_target = position_target = 0.0f;
    integral_speed = 0.0f;
}

static void accept(void)
{
    command_ms = bsp_uart_millis();
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

/* A host that keeps a run alive resends its target about ten times a second, so
   a resend must not disturb the loops. A first command, a mode change, or a
   command after a long silence drops the integrators instead. */
static bool continuous(uint32_t wanted)
{
    return mode == wanted && (uint32_t)(bsp_uart_millis() - command_ms) < CONTROL_COMMAND_MS;
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
    if (!start(CONTROL_POSITION, deg)) return false;
    position_target = deg;
    return true;
}

bool control_hold_position(void)
{
    if (!start(CONTROL_POSITION, 0.0f)) return false;
    position_target = position; /* Stop where we are, but keep holding it. */
    return true;
}

bool control_zero(void)
{
    if (foc.state != FOC_IDLE || fabsf(foc.rpm) >= 5.0f) return false;
    position = 0.0f;
    tracking = false;
    return true;
}

/* Speed PI with the position P above it. foc.rpm is already encoder-filtered. */
static float outer_output(float dt)
{
    if (mode == CONTROL_POSITION) {
        float omega = POSITION_KP * (position_target - position);
        float ceiling = 100.0f;
        if (omega > ceiling) omega = ceiling;
        if (omega < -ceiling) omega = -ceiling;
        speed_target = omega;
    }
    float error = speed_target - speed;
    float wanted = SPEED_KP * error + integral_speed;
    float limited = wanted;
    if (limited > FOC_CURRENT_MAX) limited = FOC_CURRENT_MAX;
    if (limited < -FOC_CURRENT_MAX) limited = -FOC_CURRENT_MAX;
    integral_speed += SPEED_KI * error * dt;
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
    /* At the 9400 RPM limit, one millisecond moves less than 57 degrees. */
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
    /* The outer loops hold a computed reference, so they stop when the host
       falls silent. Torque mode keeps the historical `Iq` semantics and has no
       watchdog. A host that comes back clears the watchdog fault by itself. */
    if (!control_scheduled()) { fault = FOC_UART; return; }
    if (fault == FOC_UART) fault = 0u;
    reference = outer_output(dt);
}
