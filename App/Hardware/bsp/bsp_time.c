/**
  ******************************************************************************
  * @file    bsp_time.c
  * @brief   DWT 周期计数器实现（STM32F4 / Cortex-M4）。
  *
  * 本文件是整个 App 层里少数允许 include HAL/CMSIS 的文件之一（见 App/README.md）。
  ******************************************************************************
  */

#include "bsp_time.h"

#include "stm32f4xx_hal.h"   /* 提供 CoreDebug / DWT / SystemCoreClock */

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

static bool s_dwt_ready = false;

/* 每微秒的周期数。SystemCoreClock 由 SystemInit() 在启动时按实际时钟树填好，
   本工程为 168 MHz。整数缓存一份，避免在延时循环里反复读全局变量。 */
static uint32_t s_cycles_per_us = 168u;

static void dwt_ensure_ready(void)
{
    if (!s_dwt_ready) {
        bsp_time_init();
    }
}

/* -------------------------------------------------------------------------- */
/* 公开接口                                                                    */
/* -------------------------------------------------------------------------- */

void bsp_time_init(void)
{
    /* TRCENA 是 DWT 的总开关；不开它，CYCCNT 永远不计数。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_cycles_per_us = SystemCoreClock / 1000000u;
    if (s_cycles_per_us == 0u) {
        s_cycles_per_us = 1u;   /* 防御：时钟还没配好时不要变成除零 */
    }

    s_dwt_ready = true;
}

uint32_t bsp_time_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t bsp_time_cycles_to_ns(uint32_t cycles)
{
    /* 用 64 位中间量保证精度；只在测量路径调用，不在热路径。 */
    return (uint32_t)(((uint64_t)cycles * 1000000000ULL) / (uint64_t)SystemCoreClock);
}

uint32_t bsp_time_cycles_to_us(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000000ULL) / (uint64_t)SystemCoreClock);
}

uint32_t bsp_time_us(void)
{
    return bsp_time_cycles_to_us(bsp_time_cycles());
}

void bsp_time_delay_us(uint32_t us)
{
    uint32_t start;
    uint32_t target;

    if (us == 0u) {
        return;
    }

    dwt_ensure_ready();

    /* 上限保护：us * cycles_per_us 必须能装进 32 位。
       168 MHz 下单次最多约 25.5 秒，超过就分片。 */
    if (us > 1000000u) {
        bsp_time_delay_ms(us / 1000u);
        us %= 1000u;
        if (us == 0u) {
            return;
        }
    }

    start  = DWT->CYCCNT;
    target = us * s_cycles_per_us;

    /* 无符号差值自动处理 CYCCNT 回绕，不需要额外的溢出判断。 */
    while ((uint32_t)(DWT->CYCCNT - start) < target) {
        /* 忙等 */
    }
}

void bsp_time_delay_ms(uint32_t ms)
{
    uint32_t i;

    if (ms == 0u) {
        return;
    }

    dwt_ensure_ready();

    /* 按 1 ms 分片，避免 ms * 1000 * cycles_per_us 溢出 32 位。 */
    for (i = 0u; i < ms; ++i) {
        bsp_time_delay_us(1000u);
    }
}
