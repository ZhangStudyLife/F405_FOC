#ifndef APP_MUSIC_H
#define APP_MUSIC_H

#include <stdbool.h>

/* Set to 0 and rebuild to keep the RUN-state Id target at zero. */
#define MUSIC_ENABLE 1

typedef struct { float id, iq, ud, uq; } music_sample_t;

#if MUSIC_ENABLE
/* Foreground calls require the same IRQ lock as FOC commands. */
void music_play(bool q_axis);
void music_stop(void);
bool music_active(void);
music_sample_t music_step(float iq_ref); /* 20 kHz: audio targets (A), R/L feedforward (V). */
#else
static inline void music_play(bool q_axis) { (void)q_axis; }
static inline void music_stop(void) {}
static inline bool music_active(void) { return false; }
static inline music_sample_t music_step(float iq_ref) { (void)iq_ref; return (music_sample_t){0}; }
#endif

#endif
