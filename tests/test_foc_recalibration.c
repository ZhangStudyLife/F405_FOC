#include "foc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void sample(float angle)
{
    foc_step(angle, 20.0f, 1.66f, 1.68f, 3e-6f);
}

int main(void)
{
    foc_calibration_t calibration = {0.0f, 1};
    foc_init(&calibration);
    for (unsigned n = 0; n < 6100; ++n) sample(0.0f);
    /* Acquisition continues during coast: accumulate 600 turns, then align. */
    for (unsigned n = 0; n < 600000; ++n) sample(fmodf(n * 0.36f, 360.0f));
    for (unsigned n = 0; n < 10000; ++n) sample(0.0f);
    assert(foc.state == FOC_IDLE && foc_calibrate());
    for (int n = 0; n < 120039; ++n) {
        int tick = n - 38;
        float turn = 0.0f;
        if (tick > 30000 && tick <= 70000) turn = (tick - 30000) / 40000.0f;
        if (tick > 70000 && tick <= 110000) turn = 1.0f - (tick - 70000) / 40000.0f;
        sample(turn * (360.0f / 7.0f));
    }
    assert(foc.state == FOC_SAVE && foc.calibration.direction == 1);
    puts("PASS: recalibration after 600 revolutions");
}
