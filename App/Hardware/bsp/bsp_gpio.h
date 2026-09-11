/**
  ******************************************************************************
  * @file    bsp_gpio.h
  * @brief   通用输出引脚抽象（片选、使能、指示灯等）。
  *
  * 设计要点：
  *   - 头文件里没有 HAL 类型：port 用 void* 承载，真实类型是 GPIO_TypeDef*，
  *     只有 bsp_gpio.c 和板级 port 文件知道这件事。
  *   - 带 active_low 概念：SPI 片选是低有效，调用方写语义电平（true = 选中），
  *     不用在业务代码里散落 `GPIO_PIN_RESET` 这种反直觉的字面量。
  *   - 写操作用 BSRR 单次写，不做读改写，也不会误伤同一端口的其他引脚。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_BSP_BSP_GPIO_H
#define APP_HARDWARE_BSP_BSP_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一个推挽输出引脚。 */
typedef struct {
    void    *port;        /**< 实际类型 GPIO_TypeDef*，由板级 port 文件填充 */
    uint32_t pin;         /**< GPIO_PIN_x 位掩码，可组合多引脚 */
    bool     active_low;  /**< true：write(true) 输出低电平 */
} bsp_gpio_out_t;

/**
  * @brief  把引脚配置为推挽输出，并先输出"无效"电平。
  * @note   幂等。即使 CubeMX 的 MX_GPIO_Init() 已经配过同一个脚，再配一次也无害，
  *         好处是 App 层自洽——不依赖 .ioc 里恰好配对了这个引脚。
  *         先写电平再改模式，避免配置瞬间的毛刺（片选必须先为高）。
  */
void bsp_gpio_out_init(bsp_gpio_out_t *io);

/**
  * @brief  输出逻辑电平。
  * @param  level 逻辑电平；active_low 为 true 时内部取反。
  */
void bsp_gpio_out_write(bsp_gpio_out_t *io, bool level);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_BSP_BSP_GPIO_H */
