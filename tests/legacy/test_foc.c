/* Historical voltage-mode test; incompatible with the current current-mode API. */
#include "foc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void simulate_calibration(int direction, bool stuck)
{
    foc_init(NULL);
    float start = 359.0f;
    for (unsigned n = 0; n < 120039u; ++n) {
        int t = (int)n - 38;
        float electric = 0.0f;
        if (t > 30000 && t <= 70000) electric = (float)(t - 30000) / 40000.0f;
        if (t > 70000 && t <= 110000) electric = 1.0f - (float)(t - 70000) / 40000.0f;
        float angle = fmodf(start + (stuck ? 0 : direction) * electric * (360.0f / 7.0f) + 360.0f, 360.0f);
        foc_step(angle, 12.6f);
    }
    if (stuck) { assert(foc.state == FOC_FAULT && foc.fault == FOC_ALIGNMENT); return; }
    assert(foc.state == FOC_SAVE);
    assert(foc.calibration.direction == direction);
    float error = foc_wrap(foc.calibration.zero - foc_wrap(direction * 7.0f * start * (3.141592653589793f / 180.0f)));
    assert(fminf(error, 6.283185307f - error) < 0.001f);
}

int main(void)
{
    const float turn = 6.283185307179586f;
    assert(fabsf(foc_wrap(-0.1f) - (turn - 0.1f)) < 0.00001f);
    for (unsigned n = 0; n < 3600; ++n) {
        float theta = turn * n / 3600.0f, d[3];
        foc_modulate(theta, 0.0f, 0.6f, 12.6f, d);
        assert(foc_window(d));
        float common = fmaxf(d[0], fmaxf(d[1], d[2])) + fminf(d[0], fminf(d[1], d[2]));
        assert(fabsf(common - 1.0f) < 0.00001f);
        /* Line voltages must retain the requested vector despite zero sequence. */
        float a = -0.6f * sinf(theta), b = -0.5f * a + 0.8660254038f * 0.6f * cosf(theta);
        assert(fabsf((d[0] - d[1]) * 12.6f - (a - b)) < 0.00001f);
    }
    float invalid[3] = {0.5f, 0.95f, 0.5f};
    assert(!foc_window(invalid)); invalid[1] = NAN; assert(!foc_window(invalid));
    foc_calibration_t cal = {.zero = 0.0f, .direction = 1};
    foc_init(&cal);
    assert(!foc_run(NAN) && !foc_run(INFINITY) && !foc_run(4.81f));
    assert(foc_run(0.3f));
    for (unsigned i = 0; i < 20040; ++i) foc_step(0.0f, 12.6f);
    assert(foc.state == FOC_RUN && fabsf(foc.uq - 0.3f) < 0.0001f);
    assert(!foc_run(-0.3f));
    assert(foc_run(0.0f) && foc.state == FOC_IDLE);
    foc_step(359.9f, 12.6f); foc_step(0.1f, 12.6f); assert(fabsf(foc.rpm) < 100.0f);
    foc_step(NAN, 12.6f); assert(foc.state == FOC_FAULT && foc.fault == FOC_SENSOR);
    assert(!foc_run(0.3f));
    for (unsigned bus = 6; bus <= 16; ++bus) {
        float uq = fminf(4.8f, bus * (4.8f / 12.6f));
        for (unsigned n = 0; n < 3600; ++n) {
            float d[3];
            foc_modulate(turn * n / 3600.0f, 0.0f, uq, (float)bus, d);
            assert(foc_window(d));
            foc_modulate(turn * n / 3600.0f, 0.0f, -uq, (float)bus, d);
            assert(foc_window(d));
        }
    }
    foc_init(&cal); assert(foc_run(4.8f));
    for (unsigned i = 0; i < 340000; ++i) foc_step(20.0f, 12.6f);
    assert(fabsf(foc.uq - 4.8f) < 0.0001f);
    foc_step(20.0f, 6.0f);
    assert(foc.uq <= 6.0f * (4.8f / 12.6f) && foc_window(foc.duty));
    foc_step(20.0f, 5.9f); assert(foc.fault == FOC_BUS);
    foc_init(&cal); assert(foc_run(0.3f));
    foc_step(20.0f, NAN); assert(foc.fault == FOC_BUS);
    foc_init(&cal); assert(foc_run(0.3f));
    foc_step(20.0f, 16.1f); assert(foc.fault == FOC_BUS);
    simulate_calibration(1, false);
    simulate_calibration(-1, false);
    simulate_calibration(1, true);
    puts("FOC math, wrap, ramp, window, both calibration directions and stalled rotor: PASS");
}
