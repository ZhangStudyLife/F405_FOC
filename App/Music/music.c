#include "music.h"

#if MUSIC_ENABLE
#include "motor_config.h"
#include "foc.h"
#include <math.h>

static const float notes[] = {261.625565f, 293.664768f, 329.627557f, 349.228231f,
                              391.995436f, 440.0f, 493.883301f, 523.251131f};
static unsigned note = sizeof notes / sizeof notes[0], sample;
static float phase;
static bool on_q;

void music_play(bool q_axis)
{
    note = sample = 0u;
    phase = 0.0f;
    on_q = q_axis;
}

void music_stop(void)
{
    note = sizeof notes / sizeof notes[0];
}

bool music_active(void)
{
    return note < sizeof notes / sizeof notes[0];
}

music_sample_t music_step(float iq_ref)
{
    music_sample_t output = {0};
    if (!music_active()) return output;
    if (sample < 10000u) { /* 500 ms sound, 20 ms fades at both ends. */
        float envelope = 1.0f, slope = 0.0f;
        if (sample < 400u) { envelope = (float)sample * 0.0025f; slope = 50.0f; }
        else if (sample >= 9600u) { envelope = (float)(10000u - sample) * 0.0025f; slope = -50.0f; }
        /* Motion keeps its Iq; audio uses the remaining 20 A vector budget. */
        float available = on_q ? MOTOR_CURRENT_MAX_A - fabsf(iq_ref) :
            MOTOR_CURRENT_MAX_A * MOTOR_CURRENT_MAX_A - iq_ref * iq_ref;
        float amplitude = on_q ? (available >= 2.0f ? 2.0f : available > 0.0f ? available : 0.0f) :
            (available >= 400.0f ? 20.0f : available > 0.0f ? sqrtf(available) : 0.0f);
        unsigned i = (unsigned)phase;
        float fraction = phase - (float)i;
        float current = amplitude * envelope * (foc_sine[i] + fraction * (foc_sine[i + 1u] - foc_sine[i]));
        /* Next PWM centre is about 50 us after the current sample. */
        phase += notes[note] * (512.0f / 20000.0f);
        if (phase >= 512.0f) phase -= 512.0f;
        i = (unsigned)phase;
        unsigned j = (i + 128u) & 511u;
        fraction = phase - (float)i;
        float s = foc_sine[i] + fraction * (foc_sine[i + 1u] - foc_sine[i]);
        float c = foc_sine[j] + fraction * (foc_sine[j + 1u] - foc_sine[j]);
        envelope += slope * 5e-5f; /* The piecewise fade already bounds this to [0, 1]. */
        /* Analytic derivative includes the fade; no differentiation of noisy ADC data. */
        float voltage = MOTOR_RESISTANCE_OHM * amplitude * envelope * s + MOTOR_INDUCTANCE_H * amplitude *
            (slope * s + envelope * notes[note] * 6.2831853072f * c);
        if (on_q) { output.iq = current; output.uq = voltage; }
        else { output.id = current; output.ud = voltage; }
    }
    if (++sample == 12000u) { /* 100 ms silence after each note. */
        sample = 0u;
        phase = 0.0f;
        ++note;
    }
    return output;
}
#endif
