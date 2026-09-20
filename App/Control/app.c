#include "app.h"
#include "bsp_adc.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "mt6835_port_stm32.h"
#include "justfloat.h"

bool app_init(void)
{
    if (!bsp_uart_init() || !bsp_can_init() || !mt6835_init()) return false;
    bsp_adc_start();
    return true;
}

/* Called after each encoder DMA completion, including invalid frames. */
void app_sample(void)
{
    static uint8_t divider;
    if (++divider == 20u) {
        divider = 0u;
        (void)justfloat_send(mt6835_angle_deg);
    }
}
