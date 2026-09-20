#include "foc.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define TURN (2.0f * PI)
foc_t foc;
static float target, previous, position, origin, forward, sum_sin, sum_cos, low, high;
static uint32_t ticks;
static bool tracking, aligning;

float foc_wrap(float radians)
{
    return radians - floorf(radians / TURN) * TURN;
}

void foc_modulate(float theta, float ud, float uq, float duty[3])
{
    float s = sinf(theta), c = cosf(theta);
    float a = ud * c - uq * s, beta = ud * s + uq * c;
    float b = -0.5f * a + 0.8660254038f * beta;
    float d = -0.5f * a - 0.8660254038f * beta;
    float maximum = a > b ? a : b, minimum = a < b ? a : b;
    float common = -0.5f * ((maximum > d ? maximum : d) + (minimum < d ? minimum : d));
    duty[0] = 0.5f + (a + common) / 12.6f;
    duty[1] = 0.5f + (b + common) / 12.6f;
    duty[2] = 0.5f + (d + common) / 12.6f;
}

bool foc_window(const float duty[3])
{
    /* Trigger at phase 4100, ADC aperture 28/21 MHz = 224 timer ticks.
       Reserve 84 ticks dead time + 504 ticks (3 us) analog settling on both
       sides. This engineering margin still needs loaded scope validation. */
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(duty[i]) || duty[i] < 0.05f || duty[i] > 0.95f) return false;
    float edge = (duty[1] > duty[2] ? duty[1] : duty[2]) * 4200.0f;
    return edge + 588.0f < 4100.0f && 4100.0f + 224.0f + 588.0f < 8400.0f - edge;
}

void foc_stop(void)
{
    target = foc.uq = 0.0f;
    foc.duty[0] = foc.duty[1] = foc.duty[2] = 0.0f;
    if (foc.state != FOC_FAULT) foc.state = FOC_IDLE;
}

void foc_trip(uint32_t fault)
{
    if (foc.state != FOC_FAULT) foc.fault = fault;
    foc.state = FOC_FAULT;
    foc_stop();
}

bool foc_calibrate(void)
{
    if (foc.state != FOC_IDLE) return false;
    aligning = true;
    tracking = false;
    ticks = 0u;
    foc.state = FOC_PRECHARGE;
    return true;
}

void foc_init(const foc_calibration_t *calibration)
{
    memset(&foc, 0, sizeof foc);
    tracking = false;
    target = 0.0f;
    if (calibration) {
        foc.calibration = *calibration;
        foc.calibrated = true;
    } else {
        (void)foc_calibrate();
    }
}

bool foc_run(float volts)
{
    if (!isfinite(volts) || fabsf(volts) > 0.6f) return false;
    if (volts == 0.0f) { foc_stop(); return true; }
    if (!foc.calibrated || (foc.state != FOC_IDLE && foc.state != FOC_RUN)) return false;
    if (foc.state == FOC_RUN && volts * target < 0.0f) return false;
    target = volts;
    if (foc.state == FOC_IDLE) {
        aligning = false;
        ticks = 0u;
        foc.state = FOC_PRECHARGE;
    }
    return true;
}

void foc_step(float mechanical_deg)
{
    if (!isfinite(mechanical_deg)) { foc_trip(FOC_SENSOR); return; }
    float delta = mechanical_deg - previous;
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    if (!tracking) { delta = 0.0f; position = mechanical_deg; tracking = true; }
    previous = mechanical_deg;
    position += delta;
    foc.rpm += 0.01f * (delta * (20000.0f / 6.0f) - foc.rpm);
    if (foc.state == FOC_PRECHARGE) {
        if (++ticks < 40u) return; /* 2 ms, all three low sides on. */
        ticks = 0u;
        foc.state = aligning ? FOC_CALIBRATE : FOC_RUN;
    }
    if (foc.state == FOC_RUN) {
        float step = 0.3f / 20000.0f;
        float difference = target - foc.uq;
        foc.uq += difference > step ? step : difference < -step ? -step : difference;
        float theta = foc_wrap((float)foc.calibration.direction * 7.0f * mechanical_deg * (PI / 180.0f) - foc.calibration.zero);
        foc_modulate(theta, 0.0f, foc.uq, foc.duty);
    } else if (foc.state == FOC_CALIBRATE) {
        ++ticks;
        float theta = 0.0f, ud = 0.6f; /* 0.3 V failed; 0.6 V verified on this motor. */
        if (ticks <= 10000u) ud *= (float)ticks / 10000.0f;
        if (ticks == 30000u) origin = position;
        if (ticks > 30000u && ticks <= 70000u) theta = TURN * (float)(ticks - 30000u) / 40000.0f;
        if (ticks == 70000u) {
            forward = position - origin;
            if (fabsf(forward) < (360.0f / 7.0f) * 0.8f || fabsf(forward) > (360.0f / 7.0f) * 1.2f) {
                foc_trip(FOC_ALIGNMENT); return;
            }
        }
        if (ticks > 70000u && ticks <= 110000u) theta = TURN * (1.0f - (float)(ticks - 70000u) / 40000.0f);
        if (ticks == 116000u) { sum_sin = sum_cos = 0.0f; low = high = position; }
        if (ticks > 116000u) {
            float angle = mechanical_deg * (PI / 180.0f);
            sum_sin += sinf(angle); sum_cos += cosf(angle);
            low = fminf(low, position); high = fmaxf(high, position);
        }
        if (ticks == 120000u) {
            if (fabsf(position - origin) > 1.0f || high - low > 1.0f) {
                foc_trip(FOC_ALIGNMENT); return;
            }
            foc.calibration.direction = forward > 0.0f ? 1 : -1;
            foc.calibration.zero = foc_wrap((float)foc.calibration.direction * 7.0f * atan2f(sum_sin, sum_cos));
            foc_stop();
            foc.state = FOC_SAVE;
            return;
        }
        foc_modulate(theta, ud, 0.0f, foc.duty);
    }
}
