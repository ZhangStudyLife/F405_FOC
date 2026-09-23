/* Outer-loop regression against the measured 24 V no-load plant. */
#include "bsp_uart.h"
#include "control.h"
#include "foc.h"
#include "mt6835_port_stm32.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define RADS_PER_RPM 0.1047197551f

volatile float mt6835_angle_deg = NAN, mt6835_raw_deg = NAN, mt6835_sample_delay;
volatile uint32_t mt6835_errors;
static uint32_t s_millis;
static unsigned tick20;

uint32_t bsp_uart_millis(void) { return s_millis; }
void bsp_uart_tick(void) {}
bool bsp_uart_write(const void *data, size_t size) { (void)data; return size != 0u; }

static float angle = 30.0f, rpm;
static uint32_t sample_us;
/* Independent multi-turn ground truth, fed to control_step as foc_step does. */
static float plant_position, previous_plant_angle;

/* One 20 kHz control period. torque_scale is the plant's authority, so a test
   can clamp it to inspect saturation without touching the controller. */
static void step(float torque_scale)
{
    const float dt = 50e-6f;
    float omega = rpm * RADS_PER_RPM;
    float drive = 1230.0f * foc.iq_ref * torque_scale - 0.0654f * omega - 0.27f;
    if (fabsf(omega) < 1e-4f && fabsf(drive) <= 192.5f) omega = 0.0f;
    else omega += (drive - 192.5f * (omega > 0.0f ? 1.0f : omega < 0.0f ? -1.0f : copysignf(1.0f, drive))) * dt;
    rpm = omega / RADS_PER_RPM;
    angle += rpm * 6.0f * dt; /* RPM -> degrees per second. */
    if (angle >= 360.0f) angle -= 360.0f;
    if (angle < 0.0f) angle += 360.0f;
    if (++tick20 == 20u) { ++s_millis; tick20 = 0u; } /* Monotonic, like HAL_GetTick. */
    /* The real foc_step() ends the two-millisecond PRECHARGE interlock; the
       stub does not run the current loop, so the test releases it by time. */
    if (foc.state == FOC_PRECHARGE && s_millis >= 2u) foc.state = FOC_RUN;
    float step_deg = angle - previous_plant_angle;
    if (step_deg > 180.0f) step_deg -= 360.0f;
    if (step_deg < -180.0f) step_deg += 360.0f;
    plant_position += step_deg;
    previous_plant_angle = angle;
    foc.rpm = rpm;
    if (foc.state == FOC_RUN) control_step(sample_us, plant_position);
    if (control_fault()) return;
    if (control_mode() != CONTROL_TORQUE && control_scheduled()) foc.iq_ref = control_iq_ref();
}

static void run(unsigned milliseconds, float torque_scale)
{
    for (unsigned n = 0; n < milliseconds * 20u; ++n) {
        sample_us = (sample_us + 50u) & 0xffffffu;
        step(torque_scale);
    }
}

static void reset(void)
{
    foc = (foc_t){0};
    foc.calibrated = true;
    foc.zero_ready = true;
    foc.state = FOC_IDLE;
    foc.b_offset = foc.c_offset = 1.65f;
    angle = 30.0f;
    rpm = 0.0f;
    plant_position = 0.0f;
    previous_plant_angle = angle; /* Must match, or the first delta is bogus. */
    s_millis = 0u;
    tick20 = 0u;
    control_stop();
    /* Both sides agree on the origin before each case: a real run does this
       with `zero` while the drive is idle, which is also what gates it. */
    foc.rpm = 0.0f;
    assert(control_zero());
}

static void hold(unsigned milliseconds, float torque_scale)
{
    run(milliseconds, torque_scale);
    assert(control_fault() == 0u);
}

int main(void)
{
    /* Nothing scheduled: the current loop keeps its own reference. */
    reset();
    run(50u, 0.0f);
    assert(control_iq_ref() == 0.0f && control_mode() == CONTROL_TORQUE);
    assert(fabsf(control_position_deg()) < 1e-3f);

    /* --- Speed loop: reach and hold a target. --- */
    reset();
    assert(control_speed(1000.0f));
    assert(foc.state == FOC_PRECHARGE && control_mode() == CONTROL_SPEED);
    float prior_speed = control_speed_target(), prior_accel = 0.0f;
    for (unsigned n = 0; n < 3000u; ++n) {
        hold(1u, 1.0f);
        float accel = (control_speed_target() - prior_speed) * 1000.0f;
        assert(fabsf(accel) <= 50010.0f);
        if (n) assert(fabsf(accel - prior_accel) <= 510.0f);
        prior_speed = control_speed_target(); prior_accel = accel;
    }
    assert(fabsf(control_speed_rpm() - 1000.0f) < 60.0f);
    assert(fabsf(control_iq_ref()) <= FOC_CURRENT_MAX);
    printf("speed: target 1000, measured %.1f RPM, iq %.3f A\n",
           (double)control_speed_rpm(), (double)control_iq_ref());

    /* A step down through zero keeps the sign. */
    assert(control_speed(-500.0f));
    hold(3000u, 1.0f);
    assert(fabsf(control_speed_rpm() + 500.0f) < 60.0f);
    printf("speed: target -500, measured %.1f RPM, iq %.3f A\n",
           (double)control_speed_rpm(), (double)control_iq_ref());

    /* --- Output limit: with the plant clamped the loop must saturate, not wind
       up beyond what a manual `Iq` command could ask for. --- */
    reset();
    assert(control_speed(FOC_SPEED_MAX));
    hold(2000u, 0.0f);
    assert(fabsf(control_iq_ref()) <= FOC_CURRENT_MAX);
    assert(foc.state == FOC_RUN && control_fault() == 0u);
    printf("limit: saturated at %.3f A with an immovable plant\n", (double)control_iq_ref());

    /* --- Speed and position hold their last target without host refresh. --- */
    reset();
    assert(control_speed(500.0f));
    run(400u, 0.0f);
    assert(control_scheduled() && control_fault() == 0u);
    assert(foc.state == FOC_RUN && control_speed_target() == 500.0f);
    reset();
    assert(control_position(720.0f));
    run(400u, 0.0f);
    assert(control_scheduled() && control_fault() == 0u);
    assert(foc.state == FOC_RUN && control_position_target() == 720.0f);
    /* Torque mode keeps the historical `Iq` semantics. */
    reset();
    assert(control_torque(0.5f));
    run(400u, 0.0f);
    assert(control_fault() == 0u && foc.state != FOC_FAULT);

    /* --- Position loop: converge on a multi-turn target, both directions. --- */
    reset();
    assert(control_position(720.0f)); /* Two full turns, joint-motor style. */
    hold(4000u, 1.0f);
    float error = 720.0f - plant_position;
    printf("position: target 720, reached %.2f deg, error %.2f deg, iq %.3f A\n",
           (double)plant_position, (double)error, (double)control_iq_ref());
    assert(fabsf(error) < 10.0f);
    assert(plant_position <= 750.0f); /* Overshoot bound on the way in. */

    assert(control_position(-360.0f));
    hold(4000u, 1.0f);
    error = -360.0f - plant_position;
    printf("position: target -360, reached %.2f deg, error %.2f deg\n",
           (double)plant_position, (double)error);
    assert(fabsf(error) < 10.0f);

    /* --- zero() redefines the origin and needs a stationary drive. --- */
    foc.state = FOC_IDLE;
    foc.rpm = 0.0f;
    assert(control_zero() && control_position_deg() == 0.0f);
    foc.rpm = 10.0f;
    assert(!control_zero());
    assert(control_position_deg() == 0.0f);

    /* --- stop() clears mode, targets and integrators. --- */
    reset();
    assert(control_position(180.0f));
    hold(200u, 1.0f);
    control_stop();
    assert(control_mode() == CONTROL_TORQUE && control_iq_ref() == 0.0f);
    assert(control_speed_target() == 0.0f && control_position_target() == 0.0f);
    assert(!control_scheduled());
    /* foc_step keeps unwrapping while idle. Restart after a long coast without
       treating all that idle travel as one millisecond of velocity. */
    reset();
    assert(control_speed(50.0f));
    run(20u, 0.0f);
    control_stop();
    foc.state = FOC_IDLE;
    float before_coast = control_position_deg();
    plant_position += 10000.0f;
    foc.rpm = 0.0f;
    assert(control_speed(50.0f));
    foc.state = FOC_RUN;
    for (unsigned n = 0; n < 5; ++n) {
        sample_us = (sample_us + 1000u) & 0xffffffu;
        control_step(sample_us, plant_position);
    }
    assert(fabsf(control_position_deg() - before_coast - 10000.0f) < 1.0f);
    assert(fabsf(control_speed_rpm()) < 100.0f);
    assert(fabsf(control_iq_ref()) < 1.0f);
    /* Fast jerk-limited travel and a new target while already moving. */
    reset();
    assert(control_motion(7000.0f, 50000.0f, 500000.0f));
    assert(control_position(3600.0f));
    run(200u, 1.0f);
    float previous_target = control_speed_target();
    assert(control_position(-3600.0f));
    run(1u, 1.0f);
    assert(fabsf(control_speed_target() - previous_target) < 20.0f);
    hold(3000u, 1.0f);
    assert(fabsf(plant_position + 3600.0f) < 2.0f);
    assert(fabsf(control_speed_rpm()) < 5.0f);
    puts("PASS: speed tracking, output limit, held targets, position convergence, zero, stop");
    return 0;
}
