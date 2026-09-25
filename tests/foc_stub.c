/* Minimal foc.c surface for host tests that exercise the outer loops without
   linking the full current controller. Mirrors the parts control.c reads. */
#include "foc.h"
#include <math.h>

foc_t foc;

void foc_trip(uint32_t fault)
{
    if (foc.state != FOC_FAULT) foc.fault = fault;
    foc.state = FOC_FAULT;
}

void foc_stop(void)
{
    foc.command = foc.iq_ref = foc.ud = foc.uq = 0.0f;
    if (foc.state != FOC_FAULT) foc.state = foc.zero_ready ? FOC_IDLE : FOC_OFFSET;
}

bool foc_current(float amps)
{
    if (!isfinite(amps) || fabsf(amps) > FOC_CURRENT_MAX) return false;
    if (!foc.calibrated || !foc.zero_ready) return false;
    if (foc.state != FOC_IDLE && foc.state != FOC_RUN) return false;
    foc.command = amps;
    return true;
}

bool foc_calibrate(void) { return false; }
void foc_init(const foc_calibration_t *calibration) { (void)calibration; }
float foc_wrap(float radians) { return radians; }
float foc_modulate(float alpha, float beta, float bus, float duty[3])
{
    (void)alpha; (void)beta; (void)bus; (void)duty; return 1.0f;
}
bool foc_window(const float duty[3]) { (void)duty; return true; }
void foc_step(float deg, float bus, float b, float c, float delay)
{
    (void)deg; (void)bus; (void)b; (void)c; (void)delay;
}
