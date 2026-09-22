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
#define SPEED_KI 0.02f          /* A/(RPM*s). */
/* RPM per degree. The position loop is solved by the speed loop, so its gain
   sets the damping: at 20 RPM/deg the pair is underdamped (about 0.26), at
   8 RPM/deg it is comfortably damped and 10 deg still commands 80 RPM. */
#define POSITION_KP 8.0f
#define POSITION_KI 0.0f        /* RPM/(degree*s); keep 0 to avoid windup. */
#define POSITION_KI_LIMIT 1000.0f
#define CONTROL_BANDWIDTH 0.1f  /* Integrator back-calculation gain. */
#define CONTROL_PERIOD_US 1000u /* Outer-loop period; runs once per millisecond. */
#define CONTROL_JUMP_US 4000u   /* Gap above this is a discontinuity, not a dt. */
#define CONTROL_COMMAND_MS 200u /* Stop if the host stops sending mode targets. */
/* First-order speed filter, per millisecond tick. The encoder resolves
   360/2^21 degrees, so the 1 ms position difference has a 86 RPM quantisation
   step: at alpha = 0.2 the filter runs at 100 Hz, which removes most of that
   while costing 11 degrees of phase at 1 kHz. Raise it for a faster, noisier
   loop or lower it for a smoother, slower one. */
#define CONTROL_SPEED_FILTER 0.2f

static uint32_t mode, fault;
static float position, reference, speed, speed_target, position_target, position_error;
static float integral_speed, integral_position, last_deg, previous_position;
static uint32_t previous_tick, command_ms;
static bool tracking, timed, commanded;

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
    reference = speed_target = position_target = position_error = 0.0f;
    integral_speed = integral_position = 0.0f;
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
    return mode == wanted && (uint32_t)(bsp_uart_millis() - command_ms) < 50u;
}

static bool start(uint32_t wanted, float target)
{
    if (!arm(wanted, target)) return false;
    if (!foc.calibrated || !foc.zero_ready) return false;
    if (foc.state != FOC_IDLE && foc.state != FOC_RUN && foc.state != FOC_PRECHARGE) return false;
    if (!continuous(wanted)) integral_speed = integral_position = 0.0f;
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
    return true;
}

/* Speed PI with the position P above it, or the plain reference in torque mode.
   Speed comes from the position difference over one outer period: the encoder
   resolves 360/2^21 degrees, so a whole millisecond of travel is well above the
   quantisation floor, and unlike foc.rpm there is no slow filter in the way. */
static float outer_output(float dt)
{
    if (mode == CONTROL_POSITION) {
        position_error = position_target - position;
        float omega = POSITION_KP * position_error + integral_position;
        float ceiling = FOC_SPEED_MAX * 0.9f;
        if (omega > ceiling) omega = ceiling;
        if (omega < -ceiling) omega = -ceiling;
        integral_position += POSITION_KI * position_error * dt;
        if (integral_position > POSITION_KI_LIMIT) integral_position = POSITION_KI_LIMIT;
        if (integral_position < -POSITION_KI_LIMIT) integral_position = -POSITION_KI_LIMIT;
        speed_target = omega;
    } else position_error = 0.0f;
    float error = speed_target - speed;
    float wanted = SPEED_KP * error + integral_speed;
    float limited = wanted;
    if (limited > FOC_CURRENT_MAX) limited = FOC_CURRENT_MAX;
    if (limited < -FOC_CURRENT_MAX) limited = -FOC_CURRENT_MAX;
    integral_speed += SPEED_KI * error * dt + CONTROL_BANDWIDTH * (limited - wanted);
    if (integral_speed > FOC_CURRENT_MAX) integral_speed = FOC_CURRENT_MAX;
    if (integral_speed < -FOC_CURRENT_MAX) integral_speed = -FOC_CURRENT_MAX;
    return limited;
}

void control_step(uint32_t sample_us, float mechanical_deg)
{
    if (foc.fault && !fault) fault = foc.fault; /* A trip latches this loop too. */
    if (isnan(mechanical_deg)) return;
    /* Unwrap every 20 kHz call so no single update can alias past half a turn. */
    if (!tracking) { last_deg = mechanical_deg; tracking = true; }
    float step_deg = mechanical_deg - last_deg;
    if (step_deg > 180.0f) step_deg -= 360.0f;
    if (step_deg < -180.0f) step_deg += 360.0f;
    last_deg = mechanical_deg;
    position += step_deg;
    if ((uint32_t)(sample_us - previous_tick) < CONTROL_PERIOD_US) return;
    uint32_t elapsed = (uint32_t)(sample_us - previous_tick);
    if (elapsed > CONTROL_JUMP_US) {
        previous_tick = sample_us; /* Resynchronise without integrating the gap. */
        return;
    }
    float dt = (float)elapsed * 1e-6f;
    previous_tick = sample_us;
    if (timed) {
        float raw = ((position - previous_position) / dt) * (60.0f / 360.0f);
        speed += CONTROL_SPEED_FILTER * (raw - speed);
    }
    previous_position = position;
    timed = true;
    if (!commanded) return; /* Nothing scheduled: the current loop holds iq_ref. */
    /* The outer loops hold a computed reference, so they stop when the host
       falls silent. Torque mode keeps the historical `Iq` semantics and has no
       watchdog. A host that comes back clears the watchdog fault by itself. */
    if (mode != CONTROL_TORQUE && !control_scheduled()) { fault = FOC_UART; return; }
    if (fault == FOC_UART) fault = 0u;
    reference = outer_output(dt);
}
