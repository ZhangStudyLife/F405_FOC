/**
  ******************************************************************************
  * @file    bsp_gpio.c
  * @brief   通用输出引脚实现（STM32F4 HAL / BSRR 直写）。
  *
  * 本文件是整个 App 层里少数允许 include HAL 的文件之一（见 App/README.md）。
  ******************************************************************************
  */

#include "bsp_gpio.h"

#include "stm32f4xx_hal.h"

void bsp_gpio_out_init(bsp_gpio_out_t *io)
{
    GPIO_TypeDef     *port;
    GPIO_InitTypeDef  init = {0};

    if ((io == NULL) || (io->port == NULL)) {
        return;
    }

    port = (GPIO_TypeDef *)io->port;

    /* 1) 先建立无效电平：尤其对低有效的片选，必须先为高再切成输出。 */
    HAL_GPIO_WritePin(port, io->pin,
                      io->active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* 2) 再配置为推挽输出。
          CubeMX 给 PA0（MT6835_CS）配的是 GPIO_SPEED_FREQ_LOW，对 10.5 MHz 的
          SPI 来说翻转沿偏慢、时序余量被压缩。这里统一提到 HIGH：够快，又不像
          VERY_HIGH 那样在片选这种慢线上引起过冲和 EMI。 */
    init.Pin   = io->pin;
    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &init);
}

void bsp_gpio_out_write(bsp_gpio_out_t *io, bool level)
{
    GPIO_TypeDef *port;
    uint32_t      mask;
    bool          pin_high;

    if ((io == NULL) || (io->port == NULL)) {
        return;
    }

    port     = (GPIO_TypeDef *)io->port;
    mask     = io->pin;
    pin_high = io->active_low ? (!level) : level;

    /* BSRR：低 16 位写 1 置位，高 16 位写 1 复位。
       单次 store 完成，无需关中断，也不会影响同端口其他引脚。 */
    port->BSRR = pin_high ? mask : (mask << 16u);
}
