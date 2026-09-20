#ifndef APP_BSP_MOTOR_RECORD_H
#define APP_BSP_MOTOR_RECORD_H
#include "foc.h"
#include <stddef.h>

/* On-flash ABI: sector 11, magic committed last. */
typedef struct { uint32_t version, poles; foc_calibration_t cal; uint32_t checksum, magic; } record_t;
static inline uint32_t checksum(const record_t *record)
{
    uint32_t hash = 2166136261u;
    const uint8_t *p = (const uint8_t *)record;
    for (unsigned i = 0; i < offsetof(record_t, checksum); ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
static inline bool record_valid(const record_t *r)
{
    return r->magic == 0x464f4331u && r->version == 1u && r->poles == 7u &&
        r->checksum == checksum(r) && (r->cal.direction == 1 || r->cal.direction == -1) &&
        r->cal.zero >= 0.0f && r->cal.zero < 6.2831853072f;
}
#endif
