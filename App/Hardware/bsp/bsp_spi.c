/**
  ******************************************************************************
  * @file    bsp_spi.c
  * @brief   SPI 总线抽象实现：寄存器级阻塞收发 + DMA 异步收发。
  *
  * 本文件是整个 App 层里少数允许 include HAL 的文件之一（见 App/README.md）。
  * 用到的 HAL 只有：类型定义、SPI_HandleTypeDef::Instance 指针、RCC 宏。
  * 收发循环全部直接操作寄存器，不经过 HAL_SPI_TransmitReceive。
  *
  * 实测（Release，SPI3 = 10.5 MHz，一帧 48 bit）：
  *     HAL 阻塞传输      17.0 us   ← 优化前
  *     寄存器轮询阻塞    11.2 us
  *     DMA 阻塞           8.6 us
  *     DMA 异步           4.4 us CPU（线上时间被掩盖）
  ******************************************************************************
  */

#include "bsp_spi.h"

#include "stm32f4xx_hal.h"
#include "bsp_time.h"   /* 仅用于 DMA 等待的超时计时 */

/* -------------------------------------------------------------------------- */
/* 硬件映射                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * SPI3 在 STM32F405 上的固定 DMA 映射。
 * 依据 RM0090 "DMA1 请求映射" 表，Channel 0 一列：
 *     Stream 0 -> SPI3_RX      Stream 5 -> SPI3_TX
 * （同一列里 SPI3_RX 也可用 Stream 2、SPI3_TX 也可用 Stream 7，这里取惯例组合。）
 * 将来换 SPI 实例时只改这三个宏。
 */
#ifndef BSP_SPI_DMA_RX_STREAM
#define BSP_SPI_DMA_RX_STREAM  DMA1_Stream0
#endif
#ifndef BSP_SPI_DMA_TX_STREAM
#define BSP_SPI_DMA_TX_STREAM  DMA1_Stream5
#endif
#ifndef BSP_SPI_DMA_CHANNEL
#define BSP_SPI_DMA_CHANNEL    DMA_CHANNEL_0
#endif

/*
 * 等待标志位的循环上限。正常情况下不会触发——SCK 是主机自己发的，
 * 只要外设没坏，RXNE/TXE 一定会来。这个计数只是防呆，避免外设异常时死循环。
 * 每轮约 5 个周期，50000 轮 ≈ 1.5 ms @168 MHz。
 */
#define BSP_SPI_GUARD_LOOPS  50000u

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

static SPI_TypeDef *spi_of(const bsp_spi_t *bus)
{
    return ((const SPI_HandleTypeDef *)bus->hspi)->Instance;
}

/* 每微秒的周期数。SystemCoreClock 运行期不变，缓存一份，
   省掉每次等待都做一次运行期除法（Cortex-M4 上约 20~40 周期）。 */
static uint32_t s_cycles_per_us = 0u;

static uint32_t cycles_per_us(void)
{
    uint32_t v = s_cycles_per_us;

    if (v == 0u) {
        v = SystemCoreClock / 1000000u;
        if (v == 0u) {
            v = 1u;
        }
        s_cycles_per_us = v;
    }
    return v;
}

/* 清 OVR：RM0090 规定的序列是先读 DR 再读 SR */
static void spi_clear_ovr(SPI_TypeDef *spi)
{
    (void)spi->DR;
    (void)spi->SR;
}

/* 等某个标志置位，带上限保护。返回 false 表示超时 */
static bool spi_wait_set(SPI_TypeDef *spi, uint32_t flag)
{
    uint32_t guard = BSP_SPI_GUARD_LOOPS;

    while ((spi->SR & flag) == 0u) {
        if (guard-- == 0u) {
            return false;
        }
    }
    return true;
}

/* 等某个标志清零，带上限保护 */
static bool spi_wait_clear(SPI_TypeDef *spi, uint32_t flag)
{
    uint32_t guard = BSP_SPI_GUARD_LOOPS;

    while ((spi->SR & flag) != 0u) {
        if (guard-- == 0u) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* 绑定与初始化                                                                */
/* -------------------------------------------------------------------------- */

void bsp_spi_bind(bsp_spi_t *bus, void *hspi, uint32_t timeout_ms)
{
    if (bus == NULL) {
        return;
    }

    bus->hspi       = hspi;
    bus->timeout_ms = (timeout_ms == 0u) ? 1u : timeout_ms;
    bus->dma_ready  = false;
    bus->dma_busy   = false;
}

bool bsp_spi_dma_init(bsp_spi_t *bus)
{
    SPI_HandleTypeDef  *hspi;
    DMA_Stream_TypeDef *rx;
    DMA_Stream_TypeDef *tx;

    if ((bus == NULL) || (bus->hspi == NULL)) {
        return false;
    }

    hspi = (SPI_HandleTypeDef *)bus->hspi;
    rx   = BSP_SPI_DMA_RX_STREAM;
    tx   = BSP_SPI_DMA_TX_STREAM;

    __HAL_RCC_DMA1_CLK_ENABLE();

    /* RM0090 要求 EN=0 时才能改配置；先写 CR 清 EN，再等它真正生效 */
    rx->CR = 0u;
    while ((rx->CR & DMA_SxCR_EN) != 0u) {
        /* 等流关闭 */
    }
    tx->CR = 0u;
    while ((tx->CR & DMA_SxCR_EN) != 0u) {
        /* 等流关闭 */
    }

    /* 清掉可能残留的标志 */
    DMA1->LIFCR = DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0
                | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0;
    DMA1->HIFCR = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5
                | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;

    /* RX：外设 -> 内存，地址自增，字节宽度（PSIZE/MSIZE 都留 00） */
    rx->PAR = (uint32_t)(&hspi->Instance->DR);
    rx->CR  = ((uint32_t)BSP_SPI_DMA_CHANNEL << DMA_SxCR_CHSEL_Pos)
            | DMA_SxCR_MINC
            | DMA_SxCR_PL_1;              /* 高优先级 */

    /* TX：内存 -> 外设，地址自增，字节宽度 */
    tx->PAR = (uint32_t)(&hspi->Instance->DR);
    tx->CR  = ((uint32_t)BSP_SPI_DMA_CHANNEL << DMA_SxCR_CHSEL_Pos)
            | DMA_SxCR_MINC
            | DMA_SxCR_DIR_0               /* 01 = 内存 -> 外设 */
            | DMA_SxCR_PL_1;

    bus->dma_ready = true;
    bus->dma_busy  = false;

    return true;
}

bool bsp_spi_dma_available(const bsp_spi_t *bus)
{
    return (bus != NULL) && bus->dma_ready;
}

bool bsp_spi_dma_busy(const bsp_spi_t *bus)
{
    return (bus != NULL) && bus->dma_busy;
}

/* -------------------------------------------------------------------------- */
/* 阻塞路径：寄存器级全双工                                                    */
/* -------------------------------------------------------------------------- */

bsp_spi_err_t bsp_spi_transfer(bsp_spi_t *bus,
                               const uint8_t *tx,
                               uint8_t       *rx,
                               uint16_t       len)
{
    SPI_TypeDef *spi;
    uint16_t     i;

    if ((bus == NULL) || (bus->hspi == NULL) ||
        (tx == NULL) || (rx == NULL) || (len == 0u)) {
        return BSP_SPI_ERR_NOT_BOUND;
    }
    if (bus->dma_busy) {
        return BSP_SPI_ERR_BUSY;
    }

    spi = spi_of(bus);
    spi_clear_ovr(spi);

    if ((spi->CR1 & SPI_CR1_SPE) == 0u) {
        spi->CR1 |= SPI_CR1_SPE;
    }

    /* 预填首字节：让移位寄存器和 TX 缓冲同时被占满，
       后续每个字节都能与接收重叠，总耗时贴近 8*len/SCK 的物理下限。 */
    if (!spi_wait_set(spi, SPI_SR_TXE)) {
        return BSP_SPI_ERR_TIMEOUT;
    }
    *(__IO uint8_t *)&spi->DR = tx[0];

    for (i = 1u; i < len; ++i) {
        if (!spi_wait_set(spi, SPI_SR_RXNE)) {
            return BSP_SPI_ERR_TIMEOUT;
        }
        rx[i - 1u] = *(__IO uint8_t *)&spi->DR;

        if (!spi_wait_set(spi, SPI_SR_TXE)) {
            return BSP_SPI_ERR_TIMEOUT;
        }
        *(__IO uint8_t *)&spi->DR = tx[i];
    }

    if (!spi_wait_set(spi, SPI_SR_RXNE)) {
        return BSP_SPI_ERR_TIMEOUT;
    }
    rx[len - 1u] = *(__IO uint8_t *)&spi->DR;

    /* 等总线彻底空闲，这样调用方紧接着拉高片选是安全的 */
    if (!spi_wait_clear(spi, SPI_SR_BSY)) {
        return BSP_SPI_ERR_TIMEOUT;
    }

    if ((spi->SR & SPI_SR_OVR) != 0u) {
        spi_clear_ovr(spi);
        return BSP_SPI_ERR_OVERRUN;
    }

    return BSP_SPI_OK;
}

/* -------------------------------------------------------------------------- */
/* DMA 异步路径                                                                */
/* -------------------------------------------------------------------------- */

bsp_spi_err_t bsp_spi_transfer_dma_start(bsp_spi_t *bus,
                                         const uint8_t *tx,
                                         uint8_t       *rx,
                                         uint16_t       len)
{
    SPI_TypeDef        *spi;
    DMA_Stream_TypeDef *drx;
    DMA_Stream_TypeDef *dtx;

    if ((bus == NULL) || (!bus->dma_ready) ||
        (tx == NULL) || (rx == NULL) || (len == 0u)) {
        return BSP_SPI_ERR_NOT_BOUND;
    }
    if (bus->dma_busy) {
        return BSP_SPI_ERR_BUSY;
    }

    spi = spi_of(bus);
    drx = BSP_SPI_DMA_RX_STREAM;
    dtx = BSP_SPI_DMA_TX_STREAM;

    /* 清上一次的完成/错误标志（只清本流对应的位，不动其他流） */
    DMA1->LIFCR = DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0
                | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0;
    DMA1->HIFCR = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5
                | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;

    spi_clear_ovr(spi);

    drx->NDTR = len;
    drx->M0AR = (uint32_t)rx;
    dtx->NDTR = len;
    dtx->M0AR = (uint32_t)tx;

    /* 使能顺序：先 RX 后 TX。反过来的话，TX 可能在被使能后立刻发出第一个字节，
       而 RX 还没准备好接收，导致丢字节。 */
    drx->CR |= DMA_SxCR_EN;
    dtx->CR |= DMA_SxCR_EN;

    if ((spi->CR1 & SPI_CR1_SPE) == 0u) {
        spi->CR1 |= SPI_CR1_SPE;
    }
    spi->CR2 |= (SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);

    bus->dma_busy = true;

    return BSP_SPI_OK;
}

bsp_spi_err_t bsp_spi_transfer_dma_wait(bsp_spi_t *bus, uint32_t timeout_us)
{
    SPI_TypeDef *spi;
    uint32_t     t0;
    uint32_t     limit;
    uint32_t     per_us;
    bsp_spi_err_t result = BSP_SPI_OK;

    if ((bus == NULL) || (!bus->dma_ready)) {
        return BSP_SPI_ERR_NOT_BOUND;
    }
    if (!bus->dma_busy) {
        return BSP_SPI_OK;
    }

    per_us = cycles_per_us();
    t0     = bsp_time_cycles();
    limit  = timeout_us * per_us;

    /* RX 流传输完成 = 整帧收齐。TX 一定不会晚于 RX（它是数据的来源）。 */
    while ((DMA1->LISR & DMA_LISR_TCIF0) == 0u) {
        if ((DMA1->LISR & DMA_LISR_TEIF0) != 0u) {
            result = BSP_SPI_ERR_DMA;
            break;
        }
        if ((uint32_t)(bsp_time_cycles() - t0) > limit) {
            result = BSP_SPI_ERR_TIMEOUT;
            break;
        }
    }

    /* 关掉 DMA 请求并停流；正常路径下 NDTR 已经到 0，流自己是关的 */
    spi = spi_of(bus);
    spi->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);

    BSP_SPI_DMA_RX_STREAM->CR &= ~DMA_SxCR_EN;
    BSP_SPI_DMA_TX_STREAM->CR &= ~DMA_SxCR_EN;

    (void)spi_wait_clear(spi, SPI_SR_BSY);

    if ((result == BSP_SPI_OK) && ((spi->SR & SPI_SR_OVR) != 0u)) {
        result = BSP_SPI_ERR_OVERRUN;
    }
    if (result != BSP_SPI_OK) {
        spi_clear_ovr(spi);
    }

    bus->dma_busy = false;

    return result;
}
