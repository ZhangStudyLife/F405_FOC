#include "music.h"

#if MUSIC_ENABLE
#include "motor_config.h"
#include "foc.h"
#include <math.h>
#include <string.h>

#if MUSIC_SELECTED_SONG == MUSIC_SONG_BIRTHDAY
#include "birthdaySong.inc"
#define SELECTED_SONG birthdaySong
#else
#include "Epi.inc"
#define SELECTED_SONG Epi
#endif

static const float notes[] = {261.625565f, 293.664768f, 329.627557f, 349.228231f,
                              391.995436f, 440.0f, 493.883301f, 523.251131f};
static unsigned note = sizeof notes / sizeof notes[0], sample;
static float phase;
static bool on_q;
static const music_song_t *song;
static float pcm_current, pcm_next;

const music_song_t *music_find(const char *name)
{
    return !strcmp(name, SELECTED_SONG.name) ? &SELECTED_SONG : NULL;
}

static int pcm_read(unsigned index)
{
    const uint8_t *p = song->data + (index >> 1) * 3u;
    int value = index & 1u ? (p[1] >> 4) | (p[2] << 4) : p[0] | ((p[1] & 15) << 8);
    return (value ^ 2048) - 2048;
}

static float pcm_wave(unsigned tick)
{
#if MUSIC_SAMPLE_RATE == 10000u
    unsigned i = (tick >> 1) + 2u;
    if (!(tick & 1u)) return pcm_read(i) * (1.0f / 2048.0f);
    /* Four-point interpolation at the half sample; preserve the 10 kHz timing. */
    return (-pcm_read(i - 1u) + 9 * (pcm_read(i) + pcm_read(i + 1u)) - pcm_read(i + 2u)) * (1.0f / 32768.0f);
#else
    /* Four-point Lagrange interpolation, 8 kHz -> 20 kHz (five phases). */
    static const float weights[5][4] = {
        {0.0f, 1.0f, 0.0f, 0.0f},
        {-0.048f, 0.864f, 0.216f, -0.032f},
        {-0.064f, 0.672f, 0.448f, -0.056f},
        {-0.056f, 0.448f, 0.672f, -0.064f},
        {-0.032f, 0.216f, 0.864f, -0.048f},
    };
    unsigned i = tick * 2u / 5u + 2u;
    const float *w = weights[tick * 2u % 5u];
    return (w[0] * pcm_read(i - 1u) + w[1] * pcm_read(i) +
            w[2] * pcm_read(i + 1u) + w[3] * pcm_read(i + 2u)) * (1.0f / 2048.0f);
#endif
}

void music_play_song(const music_song_t *selected)
{
    song = selected;
    sample = 0u;
    pcm_current = pcm_wave(0u);
    pcm_next = pcm_wave(1u);
}

void music_play(bool q_axis)
{
    song = NULL;
    note = sample = 0u;
    phase = 0.0f;
    on_q = q_axis;
}

void music_stop(void)
{
    song = NULL;
    note = sizeof notes / sizeof notes[0];
}

bool music_active(void)
{
    return song || note < sizeof notes / sizeof notes[0];
}

music_sample_t music_step(float iq_ref)
{
    music_sample_t output = {0};
    if (!music_active()) return output;
    if (song) {
        float available = MOTOR_CURRENT_MAX_A * MOTOR_CURRENT_MAX_A - iq_ref * iq_ref;
        float amplitude = available >= 400.0f ? 20.0f : available > 0.0f ? sqrtf(available) : 0.0f;
        float after = pcm_wave(sample + 2u);
        output.id = amplitude * pcm_current;
        /* Central difference of the band-limited waveform at the next PWM centre. */
        output.ud = amplitude * (MOTOR_RESISTANCE_OHM * pcm_next +
            MOTOR_INDUCTANCE_H * 10000.0f * (after - pcm_current));
        pcm_current = pcm_next;
        pcm_next = after;
        if (++sample == song->samples * 20u / (MUSIC_SAMPLE_RATE / 1000u)) music_stop();
        return output;
    }
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
