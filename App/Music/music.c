#include "music.h"

#if MUSIC_ENABLE
#include "motor_config.h"
#include <math.h>

static const float notes[] = {261.625565f, 293.664768f, 329.627557f, 349.228231f,
                              391.995436f, 440.0f, 493.883301f, 523.251131f};
static const float sine[513] = {
#include "../FOC/sine_table.inc"
};
static unsigned note, sample;
static float phase;
static bool playing;

void music_play(void)
{
    note = sample = 0u;
    phase = 0.0f;
    playing = true;
}

void music_stop(void)
{
    playing = false;
}

bool music_active(void)
{
    return playing;
}

float music_step(float iq_ref)
{
    if (!playing) return 0.0f;
    float current = 0.0f;
    if (sample < 10000u) { /* 500 ms sound, 20 ms fades at both ends. */
        float envelope = 1.0f;
        if (sample < 400u) envelope = (float)sample / 400.0f;
        else if (sample >= 9600u) envelope = (float)(10000u - sample) / 400.0f;
        /* Motion keeps its Iq; audio uses the remaining 20 A vector budget. */
        float available = MOTOR_CURRENT_MAX_A * MOTOR_CURRENT_MAX_A - iq_ref * iq_ref;
        float amplitude = available >= 400.0f ? 20.0f : available > 0.0f ? sqrtf(available) : 0.0f;
        unsigned i = (unsigned)phase;
        current = amplitude * envelope * (sine[i] + (phase - (float)i) * (sine[i + 1u] - sine[i]));
        phase += notes[note] * (512.0f / 20000.0f);
        if (phase >= 512.0f) phase -= 512.0f;
    }
    if (++sample == 12000u) { /* 100 ms silence after each note. */
        sample = 0u;
        phase = 0.0f;
        if (++note == sizeof notes / sizeof notes[0]) music_stop();
    }
    return current;
}
#endif
