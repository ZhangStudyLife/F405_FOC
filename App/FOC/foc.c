#include "foc.h"
#include "bsp_motor.h" /* motor_sample_us: the sample-phase timestamp. */
#include "control.h"
#include "music.h"
#include <math.h>
#include "mt6835_port_stm32.h"
#include <string.h>

#define PI 3.14159265358979323846f
#define TURN (2.0f * PI)
foc_t foc;
float foc_fault[6];
static float previous, position, origin, forward, sum_sin, sum_cos, low, high;
static uint32_t ticks, previous_sample;
static float integral_d, integral_q, variance_b, variance_c, position_roundoff;
static float proportional_d, proportional_q;
static bool tracking, aligning;

const float foc_sine[513] = {
#include "sine_table.inc"
};

static void sincos_fast(float theta, float *s, float *c)
{
    float x = theta * (512.0f / TURN);
    unsigned index = (unsigned)x;
    float fraction = x - (float)index;
    unsigned i = index & 511u, j = (i + 128u) & 511u;
    *s = foc_sine[i] + fraction * (foc_sine[i + 1u] - foc_sine[i]);
    *c = foc_sine[j] + fraction * (foc_sine[j + 1u] - foc_sine[j]);
}

static float foc_wrap(float radians)
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
    music_stop();
    foc.command = foc.iq_ref = foc.iq_audio = foc.id_ref = foc.ud = foc.uq = 0.0f;
    integral_d = integral_q = proportional_d = proportional_q = 0.0f;
    aligning = false;
    foc.duty[0] = foc.duty[1] = foc.duty[2] = 0.0f;
    control_stop();
    if (foc.state != FOC_FAULT) foc.state = foc.zero_ready ? FOC_IDLE : FOC_OFFSET;
}

void foc_trip(uint32_t fault)
{
    if (foc.state != FOC_FAULT) {
        foc.fault = fault;
        foc.state = FOC_FAULT;
        float sample[] = {foc.id, foc.iq, foc.iq_ref + foc.iq_audio, (float)(motor_timing_fault & 255u),
                          (float)((motor_timing_fault & 255u) ? motor_timing_fault >> 8 :
                                  mt6835_timing_fault == 1u ? mt6835_last_counter : 0u),
                          (float)mt6835_timing_fault};
        memcpy(foc_fault, sample, sizeof sample);
    }
    foc_stop();
}

bool foc_calibrate(void)
{
    if (foc.state != FOC_IDLE || !foc.zero_ready || fabsf(foc.rpm) >= 5.0f) return false;
    aligning = true;
    music_stop();
    foc.id_ref = 0.0f;
    foc.calibrated = false; /* An interrupted/failed attempt must not permit RUN. */
    integral_d = integral_q = proportional_d = proportional_q = 0.0f;
    position = previous; /* Keep alignment deltas precise after many revolutions. */
    position_roundoff = 0.0f;
    ticks = 0u;
    foc.state = FOC_PRECHARGE;
    return true;
}

void foc_init(const foc_calibration_t *calibration)
{
    music_stop();
    /* Current reinitialization must preserve encoder feedback and accumulation. */
    foc = (foc_t){.rpm = foc.rpm, .angle_step = foc.angle_step,
                  .id = NAN, .iq = NAN, .state = FOC_OFFSET};
    aligning = false; /* Calibration requires an explicit calibration command. */
    ticks = 0u;
    integral_d = integral_q = variance_b = variance_c = 0.0f;
    proportional_d = proportional_q = 0.0f;
    if (calibration) { foc.calibration = *calibration; foc.calibrated = true; }
}

bool foc_current(float amps)
{
    if (!isfinite(amps) || fabsf(amps) > FOC_CURRENT_MAX) return false;
    if (!foc.calibrated || !foc.zero_ready || (foc.state != FOC_IDLE && foc.state != FOC_RUN)) return false;
    foc.command = amps;
    if (foc.state == FOC_IDLE && (amps != 0.0f || music_active())) {
        aligning = false;
        integral_d = integral_q = proportional_d = proportional_q = foc.iq_ref = foc.iq_audio = 0.0f;
        ticks = 0u;
        foc.state = FOC_PRECHARGE;
    }
    return true;
}

void foc_step(float mechanical_deg, float bus_voltage, float b_voltage, float c_voltage, float encoder_delay)
{
    if (!isfinite(mechanical_deg)) { foc.angle_step = NAN; foc_trip(FOC_SENSOR); return; }
    float s, c;
    /* This encoder: repeatable second harmonic measured during unpowered coast. */
    sincos_fast(mechanical_deg * (PI / 90.0f), &s, &c);
    mechanical_deg += MOTOR_ENCODER_HARMONIC_DEG * c;
    float delta = mechanical_deg - previous;
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    uint32_t interval = tracking ? (motor_sample_us - previous_sample) & 0xffffffu : 50u;
    if (!tracking) { delta = 0.0f; position = mechanical_deg; position_roundoff = 0.0f; tracking = true; }
    /* Allow twice the rated speed, so real overspeed still reaches FOC_SPEED.
       Reject impossible 50 us jumps before Park/feedforward or speed PI sees them. */
    if ((foc.state == FOC_RUN || foc.state == FOC_CALIBRATE || foc.state == FOC_PRECHARGE) &&
        fabsf(delta) > 2.0f * FOC_SPEED_MAX * 6.0f / 20000.0f) {
        foc.angle_step = NAN;
        foc_trip(FOC_SENSOR); return;
    }
    /* Missing encoder frames have already closed the gates. On recovery,
       resample the average increment to 50 us; do not feed a whole gap into PLL. */
    foc.angle_step = delta * (50.0f / (float)interval);
    previous_sample = motor_sample_us;
    previous = mechanical_deg;
    /* Preserve sub-ULP low-speed motion after many accumulated turns. */
    float increment = delta - position_roundoff;
    float next_position = position + increment;
    position_roundoff = (next_position - position) - increment;
    position = next_position;
    foc.rpm += 0.01f * (foc.angle_step * (20000.0f / 6.0f) - foc.rpm);
    if (!isfinite(b_voltage) || !isfinite(c_voltage)) { foc_trip(FOC_ADC); return; }
    if (!isfinite(encoder_delay) || encoder_delay < 0.0f || encoder_delay > 50e-6f) {
        mt6835_timing_fault = 2u;
        foc_trip(FOC_TIMING); return;
    }
    if (foc.state == FOC_PRECHARGE || foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) {
        if (!isfinite(bus_voltage) || bus_voltage < FOC_BUS_MIN || bus_voltage > FOC_BUS_MAX) {
            foc_trip(FOC_BUS); return;
        }
    }
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
            foc.b_std_mv = sqrtf(variance_b / 2048.0f) * 1000.0f;
            foc.c_std_mv = sqrtf(variance_c / 2048.0f) * 1000.0f;
            foc.zero_complete = true;
            foc.zero_fault = (foc.b_offset < 1.4f || foc.b_offset > 1.9f) |
                ((foc.c_offset < 1.4f || foc.c_offset > 1.9f) << 1) |
                ((variance_b > 2048.0f * 16e-6f) << 2) | ((variance_c > 2048.0f * 16e-6f) << 3);
            if (foc.zero_fault) {
                foc_trip(FOC_ZERO); return;
            }
            foc.zero_ready = true;
            foc.state = FOC_IDLE;
        }
        return;
    }
    if (!foc.zero_ready) { foc.id = foc.iq = NAN; return; }
    float omega = (float)foc.calibration.direction * MOTOR_POLE_PAIRS * foc.rpm * (TURN / 60.0f);
    float theta = (float)foc.calibration.direction * MOTOR_POLE_PAIRS * mechanical_deg * (PI / 180.0f) -
                  foc.calibration.zero - omega * encoder_delay;
    /* MT6835 gives one mechanical turn: avoid floorf in the 20 kHz path. */
    theta -= (float)(int32_t)(theta * (1.0f / TURN)) * TURN;
    if (theta < 0.0f) theta += TURN;
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
        /* The speed PI must retain fast torque correction; slew torque commands only. */
        if (control.mode != CONTROL_TORQUE) foc.iq_ref = control.iq_ref;
        else {
            float step = foc.command - foc.iq_ref;
            float ramp = motor_params.current_ramp * 5e-5f;
            if (step > ramp) step = ramp;
            else if (step < -ramp) step = -ramp;
            foc.iq_ref += step;
        }
        /* A stalled speed loop must not hold the rotor at its current limit. */
        if (control.mode == CONTROL_SPEED && fabsf(control.speed_target) >= 5.0f &&
            fabsf(foc.rpm) < 5.0f && fabsf(foc.iq_ref) >= 0.9f * FOC_CURRENT_MAX) {
            if (++ticks >= 10000u) { foc_trip(FOC_STALL); return; }
        } else ticks = 0u;
        music_sample_t audio = music_step(foc.iq_ref);
        foc.id_ref = audio.id;
        foc.iq_audio = audio.iq;
        float ed = foc.id_ref - foc.id, eq = foc.iq_ref + foc.iq_audio - foc.iq;
        /* 2 kHz low-pass on P only; integral and phase-current protection stay raw. */
        proportional_d += 0.4665119f * (ed - proportional_d);
        proportional_q += 0.4665119f * (eq - proportional_q);
        float ud = motor_params.current_kp * proportional_d + integral_d + audio.ud - omega * MOTOR_INDUCTANCE_H * foc.iq;
        float uq = motor_params.current_kp * proportional_q + integral_q + audio.uq + omega * (MOTOR_INDUCTANCE_H * foc.id + MOTOR_FLUX_WB);
        /* Predict to next PWM centre (next valley + 25 us). At <=8600 RPM,
           |advance|<.32 rad: rotation error <2.9e-5, one sin/cos pair. */
        float advance = omega * ((12600.0f - FOC_HOLD_TICKS) / 168e6f);
        float a2 = advance * advance;
        float sa = advance * (1.0f - a2 / 6.0f), ca = 1.0f - a2 * (0.5f - a2 / 24.0f);
        float so = s * ca + c * sa, co = c * ca - s * sa;
        /* The ADC-window hexagon is inside the bus/sqrt(3) circle already:
           max vector = 2/3 * FOC_EDGE_LIMIT/4200 * bus. Limit only once. */
        float scale = foc_modulate(ud * co - uq * so, ud * so + uq * co, bus_voltage, foc.duty);
        foc.ud = ud * scale; foc.uq = uq * scale;
        integral_d += motor_params.current_ki * 5e-5f * ed + MOTOR_CURRENT_ANTI_WINDUP * (foc.ud - ud);
        integral_q += motor_params.current_ki * 5e-5f * eq + MOTOR_CURRENT_ANTI_WINDUP * (foc.uq - uq);
    } else if (foc.state == FOC_CALIBRATE) {
        ++ticks;
        theta = 0.0f;
        float ud = MOTOR_ALIGN_VOLTAGE_V;
        if (ticks <= 10000u) ud *= (float)ticks / 10000.0f;
        if (ticks == 26000u) low = high = position;
        if (ticks > 26000u && ticks <= 30000u) {
            low = fminf(low, position); high = fmaxf(high, position);
        }
        if (ticks == 30000u) {
            if (high - low > 1.0f) { foc_trip(FOC_ALIGNMENT); return; }
            origin = position;
            sum_sin = sum_cos = 0.0f;
        }
        if (ticks > 30000u && ticks <= 70000u) theta = TURN * (float)(ticks - 30000u) / 40000.0f;
        if (ticks == 70000u) forward = position - origin;
        if (ticks > 70000u && ticks <= 110000u) theta = TURN * (1.0f - (float)(ticks - 70000u) / 40000.0f);
        /* Check the swept trajectory, not just its endpoints. Average matched
           forward/reverse angles to reduce friction/lag bias in the zero. */
        if (ticks >= 40000u && ticks <= 110000u) {
            float direction = (ticks <= 70000u ? position - origin : forward) > 0.0f ? 1.0f : -1.0f;
            float error = direction * (position - origin) * (MOTOR_POLE_PAIRS * PI / 180.0f) - theta;
            if (fabsf(error) > PI / 6.0f) { foc_trip(FOC_ALIGNMENT); return; }
            if (ticks < 100000u && ticks % 20u == 0u) { /* 1 kHz is enough for this slow sweep. */
                float angle = foc_wrap(direction * MOTOR_POLE_PAIRS * mechanical_deg * (PI / 180.0f) - theta);
                sincos_fast(angle, &s, &c);
                sum_sin += s; sum_cos += c;
            }
        }
        if (ticks == 116000u) low = high = position;
        if (ticks > 116000u) {
            low = fminf(low, position); high = fmaxf(high, position);
        }
        if (ticks == 120000u) {
            if (fabsf(position - origin) > 1.0f || high - low > 1.0f) {
                foc_trip(FOC_ALIGNMENT); return;
            }
            float zero = atan2f(sum_sin, sum_cos);
            int direction = forward > 0.0f ? 1 : -1;
            float error = foc_wrap((float)direction * MOTOR_POLE_PAIRS * mechanical_deg * (PI / 180.0f) - zero + PI) - PI;
            if (fabsf(error) > PI / 6.0f) { foc_trip(FOC_ALIGNMENT); return; }
            foc.calibration.direction = direction;
            foc.calibration.zero = foc_wrap(zero);
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
