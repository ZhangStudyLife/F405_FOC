#ifndef APP_MUSIC_H
#define APP_MUSIC_H

#include <stdbool.h>
#include <stdint.h>
#include "music_config.h"

typedef struct { float id, iq, ud, uq; } music_sample_t;
/* Mono MUSIC_SAMPLE_RATE Hz signed 12-bit PCM, two samples per three bytes.
 * data includes two leading and at least four trailing zeros for interpolation. */
typedef struct { const char *name; const uint8_t *data; unsigned samples; } music_song_t;

#if MUSIC_ENABLE
/* Foreground playback state changes require the same IRQ lock as FOC commands. */
void music_play(bool q_axis);
const music_song_t *music_find(const char *name);
void music_play_song(const music_song_t *song);
void music_stop(void);
bool music_active(void);
music_sample_t music_step(float iq_ref); /* 20 kHz: audio targets (A), R/L feedforward (V). */
#else
static inline void music_play(bool q_axis) { (void)q_axis; }
static inline const music_song_t *music_find(const char *name) { (void)name; return 0; }
static inline void music_play_song(const music_song_t *song) { (void)song; }
static inline void music_stop(void) {}
static inline bool music_active(void) { return false; }
static inline music_sample_t music_step(float iq_ref) { (void)iq_ref; return (music_sample_t){0}; }
#endif

#endif
