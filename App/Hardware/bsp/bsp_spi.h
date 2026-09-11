/**
  ******************************************************************************
  * @file    bsp_spi.h
  * @brief   SPI 总线抽象：一次全双工收发，返回本层自己的错误码。
  *
  * 设计要点：
  *   - 头文件里没有 HAL 类型：句柄用 void* 承载，真实类型是 SPI_HandleTypeDef*。
  *   - 错误码是本层定义的枚举，不透传 HAL_StatusTypeDef，上层不需要认识 HAL。
  *   - 传输失败后主动复位 HAL 状态机。HAL 的阻塞式传输一旦超时返回，
  *     hspi->State 可能停在 BUSY，之后所有调用都会立刻失败——表现为"第一次
  *     偶发超时之后编码器就彻底读不到了"。这里把它变成可自愈。
  *
  * 关于性能（实测，非估算）：本模块提供的是阻塞式传输。在 10.5 MHz 下读 MT6835
  * 一帧 48 bit 的**线上时间只有 4.57 us**，但上板实测整个 mt6835_read() 要花
  * **17.0 us**（2856 周期 @168 MHz）——大头不是线上时间，而是 HAL 对每个字节
  * 都要走一次 SPI_WaitFlagStateUntilTimeout（6 字节 = 12 次等待）。
  * 这已占 20 kHz (50 us) 控制周期的 34%，属于"能跑但没余量"。
  *
  * 因此本模块是**性能瓶颈的所在地**，也是优化的落点。三个候选方向：
  *   1. 在本文件内实现寄存器级快路径（绕过 HAL 逐字节轮询）→ 预期约 5~6 us；
  *   2. 启用 SPI3 DMA，把 4.57 us 线上时间藏进 FOC 运算 → 预期阻塞约 2~3 us；
  *   3. 数据宽度改 16 bit，等待次数减半 → 预期约 11~12 us。
  * 取舍与具体步骤见 App/README.md 第 4 节。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_BSP_BSP_SPI_H
#define APP_HARDWARE_BSP_BSP_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SPI 传输结果。0 为成功，便于 `if (err)` 判断。 */
typedef enum {
    BSP_SPI_OK = 0,
    BSP_SPI_ERR_NOT_BOUND,   /**< 句柄为空或参数非法 */
    BSP_SPI_ERR_BUSY,        /**< 总线被占用 */
    BSP_SPI_ERR_TIMEOUT,     /**< 超时：从机没应答，通常是接线或芯片未上电 */
    BSP_SPI_ERR_OVERRUN,     /**< 接收溢出：主机没及时取走数据 */
    BSP_SPI_ERR_MODE,        /**< 模式/标志位错误（含 MODF/FRE） */
    BSP_SPI_ERR_DMA,
    BSP_SPI_ERR_OTHER
} bsp_spi_err_t;

/** @brief 一条 SPI 总线。 */
typedef struct {
    void    *hspi;        /**< 实际类型 SPI_HandleTypeDef* */
    uint32_t timeout_ms;  /**< 单次传输的 HAL 超时；建议 1 ms 量级，别用 HAL_MAX_DELAY */
} bsp_spi_t;

/**
  * @brief  绑定一条已由 CubeMX 初始化好的 SPI 总线。
  * @param  hspi        SPI_HandleTypeDef*（以 void* 传入以隔离 HAL 类型）
  * @param  timeout_ms  单次传输超时。一帧 6 字节在 10.5 MHz 下只需 5 us，
  *                     1 ms 已是三个数量级的余量；设太大只会在故障时白等。
  */
void bsp_spi_bind(bsp_spi_t *bus, void *hspi, uint32_t timeout_ms);

/**
  * @brief  全双工收发 len 个字节（阻塞）。
  * @param  tx  发送缓冲，len 字节
  * @param  rx  接收缓冲，len 字节
  * @retval BSP_SPI_OK 成功；其余为失败原因
  * @note   SPI 是全双工：想读 N 字节就必须同时写 N 字节，写 0x00 即可。
  *         本函数不操作片选，片选由调用方（驱动层）控制，这样才能保证
  *         整个帧在一次片选低电平内完成。
  */
bsp_spi_err_t bsp_spi_transfer(bsp_spi_t *bus,
                               const uint8_t *tx,
                               uint8_t       *rx,
                               uint16_t       len);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_BSP_BSP_SPI_H */
