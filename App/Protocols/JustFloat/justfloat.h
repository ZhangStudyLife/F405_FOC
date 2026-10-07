#pragma once
#include <math.h>
#include <stddef.h>

/* frame has count + 1 floats; the first count values are already filled. */
static inline size_t justfloat_pack(float *frame, size_t count)
{
    frame[count] = INFINITY;
    return (count + 1u) * sizeof *frame;
}
