#ifndef APP_PROTOCOLS_JUSTFLOAT_H
#define APP_PROTOCOLS_JUSTFLOAT_H

#include <math.h>
#include "bsp_uart.h"
#include "bsp_usb.h"

_Static_assert(sizeof(float) == 4, "JustFloat requires 32-bit floats");
/* Milliseconds + 1..16 float channels + +Inf; UART copies the frame. */
#define uart_justfloat(...) \
    (sizeof((const float[]){__VA_ARGS__}) >= sizeof(float) && \
     sizeof((const float[]){__VA_ARGS__}) <= 16u * sizeof(float) && \
     bsp_uart_write((const float[]){(float)bsp_uart_millis(), __VA_ARGS__, INFINITY}, \
                    sizeof((const float[]){__VA_ARGS__}) + 2u * sizeof(float)))

/* USB: explicit channels only, no implicit millisecond timestamp. */
#define usb_justfloat(...) \
    (sizeof((const float[]){__VA_ARGS__}) >= sizeof(float) && \
     sizeof((const float[]){__VA_ARGS__}) <= 16u * sizeof(float) && \
     bsp_usb_write((const float[]){__VA_ARGS__, INFINITY}, \
                   sizeof((const float[]){__VA_ARGS__}) + sizeof(float)))

#endif
