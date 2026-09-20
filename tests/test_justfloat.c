#include "justfloat.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t captured[72];
static size_t captured_size;
static unsigned calls;
static bool accept = true;

uint32_t bsp_uart_millis(void) { return 1234u; }

bool bsp_uart_write(const void *data, size_t size)
{
    assert(size <= sizeof captured);
    memcpy(captured, data, size);
    captured_size = size;
    calls++;
    return accept;
}

int main(void)
{
    float value = 2.0f;
    assert(justfloat_send(value++));
    assert(value == 3.0f && calls == 1 && captured_size == 12);
    const float expected[] = {1234.0f, 2.0f, INFINITY};
    assert(memcmp(captured, expected, sizeof expected) == 0);
    assert(justfloat_send(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    assert(captured_size == 72);
    assert(!justfloat_send(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, value++));
    assert(value == 3.0f && calls == 2);
    accept = false;
    assert(!justfloat_send(NAN));
    assert(calls == 3);
    float payload;
    memcpy(&payload, captured + 4, sizeof payload);
    assert(isnan(payload));
    puts("PASS: exact frame, single evaluation, channel limit, NaN, rejected send");
    return 0;
}
