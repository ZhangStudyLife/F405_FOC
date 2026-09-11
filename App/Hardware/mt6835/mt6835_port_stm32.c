/**
  ******************************************************************************
  * @file    mt6835_port_stm32.c
  * @brief   MT6835 驱动 <-> STM32 外设的适配实现（Port/Adapter 的 Adapter）。
  *
  * 职责很窄，只有三件事：
  *   1. 把 bsp_spi 的总线错误码翻译成 mt6835_status_t；
  *   2. 把 bsp_gpio 的输出引脚包装成片选回调；
  *   3. 把 bsp_time 的忙等包装成延时回调。
  *
  * 本文件是整个 App 层里少数允许 include HAL 的文件之一（见 App/README.md）。
  ******************************************************************************
  */

#include "mt6835_port_stm32.h"

#include "bsp_gpio.h"
#include "bsp_spi.h"
#include "bsp_time.h"

/* -------------------------------------------------------------------------- */
/* 每实例板级上下文                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool           used;
    bsp_spi_t      bus;
    bsp_gpio_out_t cs;
} mt6835_port_ctx_t;

static mt6835_port_ctx_t s_ctx[MT6835_PORT_MAX_INSTANCES];

static mt6835_port_ctx_t *port_ctx_alloc(void)
{
    uint32_t i;

    for (i = 0u; i < MT6835_PORT_MAX_INSTANCES; ++i) {
        if (!s_ctx[i].used) {
            s_ctx[i].used = true;
            return &s_ctx[i];
        }
    }
    return NULL;
}

static void port_ctx_free(mt6835_port_ctx_t *c)
{
    if (c != NULL) {
        c->used = false;
    }
}

/* -------------------------------------------------------------------------- */
/* 回调实现                                                                    */
/* -------------------------------------------------------------------------- */

static void port_cs_select(void *ctx, bool selected)
{
    mt6835_port_ctx_t *c = (mt6835_port_ctx_t *)ctx;

    if (c == NULL) {
        return;
    }
    /* active_low=true：selected -> 逻辑高电平 -> 引脚输出低（片选有效） */
    bsp_gpio_out_write(&c->cs, selected);
}

/* 总线错误码 -> 驱动错误码。这一层翻译是适配器的核心价值：
   驱动不需要认识 BSP，BSP 也不需要认识驱动。 */
static mt6835_status_t port_map_err(bsp_spi_err_t err)
{
    switch (err) {
        case BSP_SPI_OK:
            return MT6835_OK;
        case BSP_SPI_ERR_TIMEOUT:
            return MT6835_ERR_TIMEOUT;
        case BSP_SPI_ERR_BUSY:
            return MT6835_ERR_BUSY;
        case BSP_SPI_ERR_NOT_BOUND:
            return MT6835_ERR_NOT_INIT;
        default:
            return MT6835_ERR_BUS;
    }
}

static mt6835_status_t port_transfer(void *ctx,
                                     const uint8_t *tx,
                                     uint8_t       *rx,
                                     uint16_t       len)
{
    mt6835_port_ctx_t *c = (mt6835_port_ctx_t *)ctx;

    if (c == NULL) {
        return MT6835_ERR_BUS;
    }
    return port_map_err(bsp_spi_transfer(&c->bus, tx, rx, len));
}

/* ---- 异步（DMA）通道 ---- */

static mt6835_status_t port_transfer_start(void *ctx,
                                           const uint8_t *tx,
                                           uint8_t       *rx,
                                           uint16_t       len)
{
    mt6835_port_ctx_t *c = (mt6835_port_ctx_t *)ctx;

    if (c == NULL) {
        return MT6835_ERR_BUS;
    }
    return port_map_err(bsp_spi_transfer_dma_start(&c->bus, tx, rx, len));
}

static mt6835_status_t port_transfer_wait(void *ctx, uint32_t timeout_us)
{
    mt6835_port_ctx_t *c = (mt6835_port_ctx_t *)ctx;

    if (c == NULL) {
        return MT6835_ERR_BUS;
    }
    return port_map_err(bsp_spi_transfer_dma_wait(&c->bus, timeout_us));
}

static void port_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    bsp_time_delay_us(us);
}

static void port_delay_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    bsp_time_delay_ms(ms);
}

/* 测速用的时间源。DWT 周期计数换算成微秒，单调递增，
   约 71 分钟无符号回绕一次 —— 驱动侧的 Δt 用无符号减法，会自动处理。 */
static uint32_t port_timestamp_us(void *ctx)
{
    (void)ctx;
    return bsp_time_us();
}

/* -------------------------------------------------------------------------- */
/* 公开接口                                                                    */
/* -------------------------------------------------------------------------- */

bool mt6835_port_stm32_bind(mt6835_t          *dev,
                            SPI_HandleTypeDef *hspi,
                            GPIO_TypeDef      *cs_port,
                            uint32_t           cs_pin)
{
    mt6835_port_ctx_t *c;
    mt6835_port_t      port;

    if ((dev == NULL) || (hspi == NULL) || (cs_port == NULL) || (cs_pin == 0u)) {
        return false;
    }

    c = port_ctx_alloc();
    if (c == NULL) {
        return false;   /* 实例池耗尽：调大 MT6835_PORT_MAX_INSTANCES */
    }

    bsp_spi_bind(&c->bus, (void *)hspi, MT6835_SPI_TIMEOUT_MS);

    c->cs.port       = (void *)cs_port;
    c->cs.pin        = cs_pin;
    c->cs.active_low = true;          /* CSN 低有效 */
    bsp_gpio_out_init(&c->cs);

    port.ctx       = (void *)c;
    port.cs_select = port_cs_select;
    port.transfer  = port_transfer;
    port.delay_us  = port_delay_us;
    port.delay_ms  = port_delay_ms;
    port.timestamp_us = port_timestamp_us;   /* 测速时间源（DWT 微秒） */

    /* DMA 通道：本工程 SPI3 -> DMA1 Stream0(RX) / Stream5(TX)。
       配置全部在 bsp_spi 里用寄存器完成，不依赖 CubeMX 的 DMA 设置，
       所以改 .ioc 重新生成代码也不会把它冲掉。
       DMA 起不来时两个回调都留 NULL，驱动会自动退回阻塞路径，功能不受影响。 */
    if (bsp_spi_dma_init(&c->bus)) {
        port.transfer_start = port_transfer_start;
        port.transfer_wait  = port_transfer_wait;
    } else {
        port.transfer_start = NULL;
        port.transfer_wait  = NULL;
    }

    /* mt6835_init() 内部会拷贝一份 port，所以 port 放栈上没问题 */
    if (mt6835_init(dev, &port) != MT6835_OK) {
        port_ctx_free(c);
        return false;
    }

    return true;
}
