#ifndef APP_BSP_MOTOR_RECORD_H
#define APP_BSP_MOTOR_RECORD_H
#include "foc.h"
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t version, generation, poles;
    foc_calibration_t cal;
    motor_params_t params;
    uint32_t checksum, magic;
} motor_record_t;
typedef struct {
    uint32_t version, generation, poles;
    foc_calibration_t cal;
    float values[9];
    uint32_t checksum, magic;
} legacy_params_record_t;
typedef struct {
    uint32_t version, poles;
    foc_calibration_t cal;
    uint32_t checksum, magic;
} legacy_cal_record_t;
_Static_assert(sizeof(motor_record_t) == 56, "Flash v3 layout");

static inline uint32_t record_checksum(const void *data, size_t size)
{
    uint32_t hash = 2166136261u;
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

/* Reads v1/v2 without writing Flash. All newly saved records use v3. */
static inline bool record_decode(const void *data, motor_record_t *out)
{
    uint32_t version;
    memcpy(&version, data, sizeof version);
    if (version == 3u) {
        memcpy(out, data, sizeof *out);
        if (out->magic != 0x464f4333u || out->checksum != record_checksum(data, offsetof(motor_record_t, checksum))) return false;
    } else if (version == 2u) {
        legacy_params_record_t old;
        memcpy(&old, data, sizeof old);
        if (old.magic != 0x464f4332u || old.checksum != record_checksum(data, offsetof(legacy_params_record_t, checksum))) return false;
        *out = (motor_record_t){.version=3, .generation=old.generation, .poles=old.poles, .cal=old.cal,
            .params={old.values[0], old.values[1], old.values[2], old.values[3], old.values[4], old.values[5], old.values[6]}};
    } else if (version == 1u) {
        legacy_cal_record_t old;
        memcpy(&old, data, sizeof old);
        if (old.magic != 0x464f4331u || old.checksum != record_checksum(data, offsetof(legacy_cal_record_t, checksum))) return false;
        *out = (motor_record_t){.version=3, .poles=old.poles, .cal=old.cal, .params=MOTOR_PARAMS_DEFAULT};
    } else return false;
    return out->poles == (uint32_t)MOTOR_POLE_PAIRS && motor_params_valid(&out->params) &&
        (out->cal.direction == 1 || out->cal.direction == -1) &&
        isfinite(out->cal.zero) && out->cal.zero >= 0 && out->cal.zero < 6.2831853072f;
}
#endif
