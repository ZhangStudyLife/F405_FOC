#include "justfloat.h"
#include "bsp_uart.h"
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "JustFloat requires 32-bit floats");

bool justfloat_array(const float *values, size_t count)
{
    float frame[18];
    const uint32_t tail = 0x7f800000u;
    if (values == NULL || count == 0u || count > 16u) return false;
    frame[0] = (float)bsp_uart_millis();
    for (size_t i = 0u; i < count; ++i) memcpy(frame + i + 1u, values + i, 4u);
    memcpy(frame + count + 1u, &tail, sizeof tail);
    return bsp_uart_write(frame, (count + 2u) * sizeof(float));
}
