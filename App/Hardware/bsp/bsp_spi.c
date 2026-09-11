/**
  ******************************************************************************
  * @file    bsp_spi.c
  * @brief   SPI 总线抽象实现（STM32F4 HAL）。
  *
  * 本文件是整个 App 层里少数允许 include HAL 的文件之一（见 App/README.md）。
  ******************************************************************************
  */

#include "bsp_spi.h"

#include "stm32f4xx_hal.h"

void bsp_spi_bind(bsp_spi_t *bus, void *hspi, uint32_t timeout_ms)
{
    if (bus == NULL) {
        return;
    }

    bus->hspi       = hspi;
    bus->timeout_ms = (timeout_ms == 0u) ? 1u : timeout_ms;
}

bsp_spi_err_t bsp_spi_transfer(bsp_spi_t *bus,
                               const uint8_t *tx,
                               uint8_t       *rx,
                               uint16_t       len)
{
    SPI_HandleTypeDef *hspi;
    HAL_StatusTypeDef  hal;
    uint32_t           err;

    if ((bus == NULL) || (bus->hspi == NULL) ||
        (tx == NULL) || (rx == NULL) || (len == 0u)) {
        return BSP_SPI_ERR_NOT_BOUND;
    }

    hspi = (SPI_HandleTypeDef *)bus->hspi;

    /* 上一次异常退出留下的 BUSY 会让之后每一次调用都直接失败，
       表现成"偶发一次超时之后编码器永久读不到"。这里主动清一次。 */
    if (hspi->State != HAL_SPI_STATE_READY) {
        (void)HAL_SPI_Abort(hspi);
    }

    hal = HAL_SPI_TransmitReceive(hspi, (uint8_t *)tx, rx, len, bus->timeout_ms);

    if (hal == HAL_OK) {
        return BSP_SPI_OK;
    }

    /* 取错误码要在 Abort 之前，Abort 会清 ErrorCode。 */
    err = HAL_SPI_GetError(hspi);
    (void)HAL_SPI_Abort(hspi);

    if (hal == HAL_TIMEOUT) {
        return BSP_SPI_ERR_TIMEOUT;
    }
    if ((err & HAL_SPI_ERROR_OVR) != 0u) {
        return BSP_SPI_ERR_OVERRUN;
    }
    if ((err & HAL_SPI_ERROR_DMA) != 0u) {
        return BSP_SPI_ERR_DMA;
    }
    if ((err & (HAL_SPI_ERROR_MODF | HAL_SPI_ERROR_FRE | HAL_SPI_ERROR_FLAG)) != 0u) {
        return BSP_SPI_ERR_MODE;
    }
    if (hal == HAL_BUSY) {
        return BSP_SPI_ERR_BUSY;
    }

    return BSP_SPI_ERR_OTHER;
}
