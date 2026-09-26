#include "foc.h"
#include "bsp_motor.h" /* motor_sample_us: the sample-phase timestamp. */
#include "control.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define TURN (2.0f * PI)
foc_t foc;
static float previous, position, origin, forward, sum_sin, sum_cos, low, high;
static uint32_t ticks;
static float integral_d, integral_q, variance_b, variance_c, previous_command;
static bool tracking, aligning;

static void sincos_fast(float theta, float *s, float *c)
{
    static const float table[513] = {
#include "sine_table.inc"
    };
    float x = theta * (512.0f / TURN);
    unsigned index = (unsigned)x;
    float fraction = x - (float)index;
    unsigned i = index & 511u, j = (i + 128u) & 511u;
    *s = table[i] + fraction * (table[i + 1u] - table[i]);
    *c = table[j] + fraction * (table[j + 1u] - table[j]);
}

float foc_wrap(float radians)
{
    return radians - floorf(radians / TURN) * TURN;
}

float foc_modulate(float a, float beta, float bus_voltage, float duty[3])
{
    float b = -0.5f * a + 0.8660254038f * beta;
    float c = -0.5f * a - 0.8660254038f * beta;
    float maximum = a > b ? a : b, minimum = a < b ? a : b;
    maximum = maximum > c ? maximum : c;
    minimum = minimum < c ? minimum : c;
    /* Keep centred SVPWM where possible. Shift all phases equally to extend
       the low-side window; scale only when the phase span no longer fits. */
    float ceiling = (float)FOC_EDGE_LIMIT / 4200.0f;
    float span = maximum - minimum, available = ceiling * bus_voltage;
    float scale = span > available ? available / span : 1.0f;
    float inverse_bus = 1.0f / bus_voltage;
    float width = span * scale * inverse_bus;
    float offset = 0.5f - 0.5f * width;
    if (offset > ceiling - width) offset = ceiling - width;
    if (offset < 0.0f) offset = 0.0f; /* Roundoff at full phase span. */
    inverse_bus *= scale;
    duty[0] = offset + (a - minimum) * inverse_bus;
    duty[1] = offset + (b - minimum) * inverse_bus;
    duty[2] = offset + (c - minimum) * inverse_bus;
    return scale;
}

bool foc_window(const float duty[3])
{
    /* All phases must be quiet across the aperture, B/C low sides conducting.
       Include quantized CCR rounding and the provisional trigger allowance. */
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(duty[i]) || duty[i] < 0.0f || duty[i] > 1.0f) return false;
        uint32_t edge = (uint32_t)(duty[i] * 4200.0f + 0.5f);
        if (edge + FOC_SETTLE_TICKS >= FOC_TRIGGER_TICKS ||
            FOC_HOLD_TICKS + FOC_SETTLE_TICKS >= 8400u - edge) return false;
    }
    return true;
}

void foc_stop(void)
{
    foc.command = foc.iq_ref = foc.ud = foc.uq = 0.0f;
    integral_d = integral_q = 0.0f;
    aligning = false;
    previous_command = 0.0f; /* Cancel any in-flight command ramp. */
    foc.duty[0] = foc.duty[1] = foc.duty[2] = 0.0f;
    control_stop();
    if (foc.state != FOC_FAULT) foc.state = foc.zero_ready ? FOC_IDLE : FOC_OFFSET;
}

void foc_trip(uint32_t fault)
{
    if (foc.state != FOC_FAULT) foc.fault = fault;
    foc.state = FOC_FAULT;
    foc_stop();
}

bool foc_calibrate(void)
{
    if (foc.state != FOC_IDLE || !foc.zero_ready || fabsf(foc.rpm) >= 5.0f) return false;
    aligning = true;
    integral_d = integral_q = 0.0f;
    position = previous; /* Keep alignment deltas precise after many revolutions. */
    ticks = 0u;
    foc.state = FOC_PRECHARGE;
    return true;
}

void foc_init(const foc_calibration_t *calibration)
{
    memset(&foc, 0, sizeof foc);
    tracking = false;
    aligning = calibration == NULL; /* Boot-only automatic alignment, cancelled by stop. */
    ticks = 0u;
    integral_d = integral_q = variance_b = variance_c = 0.0f;
    if (calibration) { foc.calibration = *calibration; foc.calibrated = true; }
    foc.state = FOC_OFFSET; /* Gate-off current offsets precede any alignment. */
}

bool foc_current(float amps)
{
    if (!isfinite(amps) || fabsf(amps) > 5.0f) return false;
    if (!foc.calibrated || !foc.zero_ready || (foc.state != FOC_IDLE && foc.state != FOC_RUN)) return false;
    foc.command = amps;
    if (foc.state == FOC_IDLE && amps != 0.0f) {
        aligning = false;
        integral_d = integral_q = foc.iq_ref = 0.0f;
        ticks = 0u;
        foc.state = FOC_PRECHARGE;
    }
    return true;
}

void foc_step(float mechanical_deg, float bus_voltage, float b_voltage, float c_voltage, float encoder_delay)
{
    if (!isfinite(mechanical_deg)) { foc_trip(FOC_SENSOR); return; }
    if (!isfinite(b_voltage) || !isfinite(c_voltage)) { foc_trip(FOC_ADC); return; }
    if (!isfinite(encoder_delay) || encoder_delay < 0.0f || encoder_delay > 50e-6f) {
        foc_trip(FOC_TIMING); return;
    }
    if (foc.state == FOC_PRECHARGE || foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) {
        if (!isfinite(bus_voltage) || bus_voltage < FOC_BUS_MIN || bus_voltage > FOC_BUS_MAX) {
            foc_trip(FOC_BUS); return;
        }
    }
    float s, c;
    /* This encoder: repeatable second harmonic measured during unpowered coast. */
    sincos_fast(mechanical_deg * (PI / 90.0f), &s, &c);
    mechanical_deg += MOTOR_ENCODER_HARMONIC_DEG * c;
    float delta = mechanical_deg - previous;
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    if (!tracking) { delta = 0.0f; position = mechanical_deg; tracking = true; }
    previous = mechanical_deg;
    position += delta;
    foc.rpm += 0.01f * (delta * (20000.0f / 6.0f) - foc.rpm);
    if (foc.state == FOC_OFFSET) {
        if (fabsf(foc.rpm) >= 5.0f || fabsf(delta) >= 5.0f * 6.0f / 20000.0f) {
            ticks = 0u;
            foc.b_offset = foc.c_offset = variance_b = variance_c = 0.0f;
            return;
        }
        if (++ticks <= 4000u) return; /* 200 ms continuously stationary. */
        float n = (float)(ticks - 4000u);
        float db = b_voltage - foc.b_offset, dc = c_voltage - foc.c_offset;
        foc.b_offset += db / n; foc.c_offset += dc / n;
        variance_b += db * (b_voltage - foc.b_offset);
        variance_c += dc * (c_voltage - foc.c_offset);
        if (ticks == 6048u) {
            if (foc.b_offset < 1.4f || foc.b_offset > 1.9f || foc.c_offset < 1.4f || foc.c_offset > 1.9f ||
                variance_b > 2048.0f * 4e-6f || variance_c > 2048.0f * 4e-6f) {
                foc_trip(FOC_ZERO); return;
            }
            foc.zero_ready = true;
            foc.state = FOC_IDLE;
            if (aligning && !foc.calibrated) (void)foc_calibrate();
        }
        return;
    }
    if (!foc.zero_ready) { foc.id = foc.iq = 0.0f; return; }
    float omega = (float)foc.calibration.direction * MOTOR_POLE_PAIRS * foc.rpm * (TURN / 60.0f);
    float theta = foc_wrap((float)foc.calibration.direction * MOTOR_POLE_PAIRS * mechanical_deg * (PI / 180.0f) -
                          foc.calibration.zero - omega * encoder_delay);
    foc.electrical_deg = theta * (180.0f / PI);
    sincos_fast(theta, &s, &c);
    float ib = (b_voltage - foc.b_offset) * 50.0f, ic = (c_voltage - foc.c_offset) * 50.0f;
    float ia = -ib - ic, beta = (ib - ic) * 0.5773502692f;
    foc.id = ia * c + beta * s;
    foc.iq = -ia * s + beta * c;
    if (foc.state == FOC_PRECHARGE || foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) {
        if (fabsf(ia) >= MOTOR_PHASE_CURRENT_TRIP_A || fabsf(ib) >= MOTOR_PHASE_CURRENT_TRIP_A ||
            fabsf(ic) >= MOTOR_PHASE_CURRENT_TRIP_A) { foc_trip(FOC_CURRENT); return; }
        if (!aligning && fabsf(foc.rpm) >= FOC_SPEED_MAX) { foc_trip(FOC_SPEED); return; }
    }
    if (foc.state == FOC_PRECHARGE) {
        if (++ticks < 40u) return; /* 2 ms, all three low sides on. */
        ticks = 0u;
        foc.state = aligning ? FOC_CALIBRATE : FOC_RUN;
    }
    if (foc.state == FOC_RUN) {
        /* 10 A/s command ramp, slewed at the fixed 20 kHz control rate. */
        float step = foc.command - previous_command;
        if (step > 5e-4f) step = 5e-4f;
        else if (step < -5e-4f) step = -5e-4f;
        previous_command += step;
        foc.iq_ref = previous_command;
        /* The 1 kHz outer loop, when scheduled, replaces the reference. It only
           produces a value on its own millisecond, so hold the torque reference
           on the nineteen samples in between. */
        uint32_t outer_fault = control_fault();
        if (outer_fault) { foc_trip(outer_fault); return; }
        if (control_mode() != CONTROL_TORQUE && control_scheduled())
            foc.iq_ref = control_iq_ref();
        /* 600 Hz PI, R=.12 ohm, L=50 uH; no feedback low-pass.
           Back calculation Tt=L/R. Feedforward uses nominal motor parameters. */
        float ed = -foc.id, eq = foc.iq_ref - foc.iq;
        float ud = MOTOR_CURRENT_KP * ed + integral_d - omega * MOTOR_INDUCTANCE_H * foc.iq;
        float uq = MOTOR_CURRENT_KP * eq + integral_q + omega * (MOTOR_INDUCTANCE_H * foc.id + MOTOR_FLUX_WB);
        /* Linear SVPWM ceiling; modulation also accounts for the ADC window. */
        float limit = bus_voltage * 0.5773502692f;
        float norm2 = ud * ud + uq * uq;
        float scale = norm2 > limit * limit ? limit / sqrtf(norm2) : 1.0f;
        foc.ud = ud * scale; foc.uq = uq * scale;
        /* Predict to next PWM centre (next valley + 25 us). At <=8600 RPM,
           |advance|<.32 rad: rotation error <2.9e-5, one sin/cos pair. */
        float advance = omega * ((12600.0f - FOC_HOLD_TICKS) / 168e6f);
        float a2 = advance * advance;
        float sa = advance * (1.0f - a2 / 6.0f), ca = 1.0f - a2 * (0.5f - a2 / 24.0f);
        float so = s * ca + c * sa, co = c * ca - s * sa;
        scale = foc_modulate(foc.ud * co - foc.uq * so, foc.ud * so + foc.uq * co, bus_voltage, foc.duty);
        foc.ud *= scale; foc.uq *= scale;
        integral_d += MOTOR_CURRENT_KI_TS * ed + MOTOR_CURRENT_ANTI_WINDUP * (foc.ud - ud);
        integral_q += MOTOR_CURRENT_KI_TS * eq + MOTOR_CURRENT_ANTI_WINDUP * (foc.uq - uq);
    } else if (foc.state == FOC_CALIBRATE) {
        ++ticks;
        float theta = 0.0f, ud = MOTOR_ALIGN_VOLTAGE_V;
        if (ticks <= 10000u) ud *= (float)ticks / 10000.0f;
        if (ticks == 30000u) origin = position;
        if (ticks > 30000u && ticks <= 70000u) theta = TURN * (float)(ticks - 30000u) / 40000.0f;
        if (ticks == 70000u) {
            forward = position - origin;
            if (fabsf(forward) < (360.0f / MOTOR_POLE_PAIRS) * 0.8f ||
                fabsf(forward) > (360.0f / MOTOR_POLE_PAIRS) * 1.2f) {
                foc_trip(FOC_ALIGNMENT); return;
            }
        }
        if (ticks > 70000u && ticks <= 110000u) theta = TURN * (1.0f - (float)(ticks - 70000u) / 40000.0f);
        if (ticks == 116000u) { sum_sin = sum_cos = 0.0f; low = high = position; }
        if (ticks > 116000u) {
            float angle = mechanical_deg * (PI / 180.0f);
            sincos_fast(angle, &s, &c);
            sum_sin += s; sum_cos += c;
            low = fminf(low, position); high = fmaxf(high, position);
        }
        if (ticks == 120000u) {
            if (fabsf(position - origin) > 1.0f || high - low > 1.0f) {
                foc_trip(FOC_ALIGNMENT); return;
            }
            foc.calibration.direction = forward > 0.0f ? 1 : -1;
            foc.calibration.zero = foc_wrap((float)foc.calibration.direction * MOTOR_POLE_PAIRS *
                                            atan2f(sum_sin, sum_cos));
            foc_stop();
            foc.state = FOC_SAVE;
            return;
        }
        foc.ud = ud; foc.uq = 0.0f;
        sincos_fast(theta, &s, &c);
        foc_modulate(ud * c, ud * s, bus_voltage, foc.duty);
    }
}

void foc_outer_step(void)
{
    control_step(motor_sample_us, position);
}
