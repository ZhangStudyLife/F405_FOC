#ifndef APP_PROTOCOLS_JUSTFLOAT_H
#define APP_PROTOCOLS_JUSTFLOAT_H

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include "bsp_uart.h"

/* 1..16 business channels, preceded by float milliseconds, followed by +Inf. */
bool justfloat_array(const float *values, size_t count);
/* Constant-size frame: no varargs promotion, no intermediate payload copy. */
#define justfloat_send(...) \
    (sizeof((const float[]){__VA_ARGS__}) >= sizeof(float) && \
     sizeof((const float[]){__VA_ARGS__}) <= 16u * sizeof(float) && \
     bsp_uart_write((const float[]){(float)bsp_uart_millis(), __VA_ARGS__, INFINITY}, \
                    sizeof((const float[]){__VA_ARGS__}) + 2u * sizeof(float)))

#endif
