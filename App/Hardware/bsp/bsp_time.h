/**
  ******************************************************************************
  * @file    bsp_time.h
  * @brief   时间基准：DWT 周期计数、微秒时戳、忙等延时。
  *
  * 这一层是本工程"时间"概念的唯一入口。上层（驱动程序、FOC）只认 bsp_time_*，
  * 不直接碰 SysTick / DWT / HAL_Delay，换芯片时只改 bsp_time.c。
  *
  * 与 HAL_Delay 的区别：
  *   - HAL_Delay 依赖 SysTick 中断，最小分辨率 1 ms，且关中断时完全失效；
  *   - 本模块用 Cortex-M4 的 DWT->CYCCNT，1 周期分辨率，关中断也能用。
  *     20 kHz 控制周期只有 50 us，任何 ms 级 API 都不够用。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_BSP_BSP_TIME_H
#define APP_HARDWARE_BSP_BSP_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  使能 DWT 周期计数器。
  * @note   必须在 main() 中、进入实时控制之前调用一次。
  *         可重复调用，幂等。bsp_time_delay_us/ms 会在未初始化时自动补一次，
  *         但 bsp_time_cycles() 不会（热路径里不引入分支）。
  */
void bsp_time_init(void);

/**
  * @brief  读取 CPU 周期计数。
  * @retval 自 bsp_time_init() 起累计的周期数，32 位自然回绕。
  * @note   168 MHz 下约 25.6 秒回绕一次。**只用于测量短区间**：
  *         两个时戳相减请用无符号差值 `t1 - t0`，它会自动正确处理回绕。
  */
uint32_t bsp_time_cycles(void);

/** @brief 周期数 -> 纳秒（整数运算，无浮点）。 */
uint32_t bsp_time_cycles_to_ns(uint32_t cycles);

/** @brief 周期数 -> 微秒（整数运算，无浮点）。 */
uint32_t bsp_time_cycles_to_us(uint32_t cycles);

/** @brief 自 bsp_time_init() 起经过的微秒数（32 位约 71 分钟回绕）。 */
uint32_t bsp_time_us(void);

/**
  * @brief  忙等延时（微秒）。
  * @note   不依赖任何中断，可在关中断状态下使用。
  *         只适合 1 us ~ 数十 ms 的短延时；长时间等待请用状态机而不是阻塞。
  */
void bsp_time_delay_us(uint32_t us);

/** @brief 忙等延时（毫秒）。实现为分片调用 delay_us，避免长延时期间的乘法溢出。 */
void bsp_time_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_BSP_BSP_TIME_H */
