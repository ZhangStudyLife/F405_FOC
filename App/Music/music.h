#ifndef APP_MUSIC_H
#define APP_MUSIC_H

#include <stdbool.h>

/* Set to 0 and rebuild to keep the RUN-state Id target at zero. */
#define MUSIC_ENABLE 1

#if MUSIC_ENABLE
/* Foreground calls require the same IRQ lock as FOC commands. */
void music_play(void);
void music_stop(void);
bool music_active(void);
float music_step(float iq_ref); /* Once per 20 kHz RUN cycle; returns Id in A. */
#else
static inline void music_play(void) {}
static inline void music_stop(void) {}
static inline bool music_active(void) { return false; }
static inline float music_step(float iq_ref) { (void)iq_ref; return 0.0f; }
#endif

#endif
