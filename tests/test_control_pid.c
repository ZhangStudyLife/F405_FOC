/* Outer-loop regression: the real 1 kHz scheduler against a first-order plant.
   The plant is a torque constant and an inertia, so the settling behaviour is
   analytic rather than fitted to the controller under test. Every long run
   refreshes the target every 100 ms, which is how the host keeps the watchdog
   satisfied; a silent host is a separate, deliberate case below. */
#include "bsp_uart.h"
#include "control.h"
#include "foc.h"
#include "mt6835_port_stm32.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* 5010-KV360: Kt = 60/(2*pi*360) N*m/A. The rotor inertia is not in the motor
   documentation, so the test uses a small outrunner value; it only sets the
   timescale, and it is what makes the settling measurements below meaningful. */
#define KT 0.0265258238f
#define INERTIA 5e-6f
#define RADS_PER_RPM 0.1047197551f

volatile float mt6835_angle_deg = NAN, mt6835_raw_deg = NAN, mt6835_sample_delay;
volatile uint32_t mt6835_errors;
static uint32_t s_millis;

uint32_t bsp_uart_millis(void) { return s_millis; }
void bsp_uart_tick(void) {}
bool bsp_uart_write(const void *data, size_t size) { (void)data; return size != 0u; }

static float angle = 30.0f, rpm;
static uint32_t sample_us;
/* Independent multi-turn ground truth: the same unwrap as control.c, but driven
   here from the plant's own angle rather than from the loop under test. */
static float plant_position, previous_plant_angle;

/* One 20 kHz control period. torque_scale is the plant's authority, so a test
   can clamp it to inspect saturation without touching the controller. */
static void step(float torque_scale)
{
    const float dt = 50e-6f;
    rpm += (KT * foc.iq_ref * torque_scale / INERTIA) * RADS_PER_RPM * dt;
    angle += rpm * 6.0f * dt; /* RPM -> degrees per second. */
    if (angle >= 360.0f) angle -= 360.0f;
    if (angle < 0.0f) angle += 360.0f;
    if (sample_us % 1000u == 0u) ++s_millis; /* Monotonic, like HAL_GetTick. */
    /* The real foc_step() ends the two-millisecond PRECHARGE interlock; the
       stub does not run the current loop, so the test releases it by time. */
    if (foc.state == FOC_PRECHARGE && s_millis >= 2u) foc.state = FOC_RUN;
    control_step(sample_us, angle);
    if (control_fault()) return;
    float step_deg = angle - previous_plant_angle;
    if (step_deg > 180.0f) step_deg -= 360.0f;
    if (step_deg < -180.0f) step_deg += 360.0f;
    plant_position += step_deg;
    previous_plant_angle = angle;
    if (control_mode() != CONTROL_TORQUE && control_scheduled()) foc.iq_ref = control_iq_ref();
}

static void run(unsigned milliseconds, float torque_scale)
{
    for (unsigned n = 0; n < milliseconds * 20u; ++n) {
        sample_us = (sample_us + 50u) % 1000000u;
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
    control_stop();
    /* Both sides agree on the origin before each case: a real run does this
       with `zero` while the drive is idle, which is also what gates it. */
    foc.rpm = 0.0f;
    assert(control_zero());
}

/* Mode commands are the host's keepalive: resent every 100 ms while running. */
static void hold(unsigned milliseconds, float torque_scale)
{
    unsigned elapsed = 0u;
    while (elapsed < milliseconds) {
        assert(control_fault() == 0u);
        if (control_mode() == CONTROL_SPEED) assert(control_speed(control_speed_target()));
        else if (control_mode() == CONTROL_POSITION) assert(control_position(control_position_target()));
        else assert(control_torque(foc.command));
        run(100u, torque_scale);
        elapsed += 100u;
    }
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
    hold(3000u, 1.0f);
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

    /* --- Host watchdog: a held reference demands a live command stream. --- */
    reset();
    assert(control_speed(500.0f));
    run(180u, 0.0f);
    assert(control_scheduled() && control_fault() == 0u);
    run(40u, 0.0f); /* 220 ms since the last command. */
    assert(control_fault() == FOC_UART);
    printf("watchdog: tripped with FOC_UART after 220 ms of silence\n");
    /* Torque mode keeps the historical `Iq` semantics and has no watchdog. */
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
    puts("PASS: speed tracking, output limit, host watchdog, position convergence, zero, stop");
    return 0;
}
