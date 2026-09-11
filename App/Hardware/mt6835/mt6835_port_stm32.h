/**
  ******************************************************************************
  * @file    mt6835_port_stm32.h
  * @brief   把 MT6835 驱动接到 STM32 的 SPI + GPIO 上（板级适配层）。
  *
  * 这是应用层里**唯一**知道"MT6835 挂在 SPI3、片选是 PA0"的地方。
  * 驱动本体（mt6835.c/.h）完全不知道 STM32 的存在。
  *
  * 因此本头文件允许 include HAL；除此之外的 App 代码不应该 include 它，
  * 只有 main.c 的初始化段落需要。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_MT6835_MT6835_PORT_STM32_H
#define APP_HARDWARE_MT6835_MT6835_PORT_STM32_H

#include <stdint.h>
#include <stdbool.h>

#include "stm32f4xx_hal.h"

#include "mt6835.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief 单帧 SPI 超时（ms）。
  * @note  6 字节 @10.5 MHz 线上时间只有约 5 us，1 ms 已是三个数量级余量。
  *        千万不要用 HAL_MAX_DELAY：芯片掉线时会把控制中断永久挂死。
  */
#define MT6835_SPI_TIMEOUT_MS 1u

/** @brief 同时支持的 MT6835 实例数（多编码器场景）。可用 CMake 覆盖。 */
#ifndef MT6835_PORT_MAX_INSTANCES
#define MT6835_PORT_MAX_INSTANCES 2u
#endif

/**
  * @brief  绑定 SPI 总线与片选引脚，并完成 mt6835_init() 建链。
  * @param  dev      驱动实例
  * @param  hspi     已由 CubeMX 初始化好的 SPI 句柄，本工程为 &hspi3
  * @param  cs_port  片选所在端口，本工程为 MT6835_CS_GPIO_Port（GPIOA）
  * @param  cs_pin   片选引脚掩码，本工程为 MT6835_CS_Pin（GPIO_PIN_0）
  * @retval true   建链成功，可以开始 mt6835_read()
  * @retval false  参数非法、实例池耗尽，或芯片无应答
  *
  * @note   片选按**低有效**处理（CSN）。
  *         调用失败时不会调用 Error_Handler()，由调用方决定降级策略。
  */
bool mt6835_port_stm32_bind(mt6835_t          *dev,
                            SPI_HandleTypeDef *hspi,
                            GPIO_TypeDef      *cs_port,
                            uint32_t           cs_pin);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_MT6835_MT6835_PORT_STM32_H */
