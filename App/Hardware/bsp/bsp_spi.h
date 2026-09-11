/**
  ******************************************************************************
  * @file    bsp_spi.h
  * @brief   SPI 总线抽象：寄存器级快速收发 + DMA 异步收发。
  *
  * 设计要点：
  *   - 头文件里没有 HAL 类型：句柄用 void* 承载，真实类型是 SPI_HandleTypeDef*。
  *   - 错误码是本层定义的枚举，不透传 HAL_StatusTypeDef，上层不需要认识 HAL。
  *   - 收发走**寄存器级实现**，不调 HAL_SPI_TransmitReceive。
  *
  * ---------------------------------------------------------------------------
  * 实测对比（Release，SPI3 = 10.5 MHz，读 MT6835 一帧 48 bit）
  * ---------------------------------------------------------------------------
  *   SPI 线上时间（物理下限）        4.19 us
  *   HAL_SPI_TransmitReceive 阻塞    17.0 us   ← 最初实现，占 20 kHz 周期 34%
  *   bsp_spi_transfer 寄存器轮询     11.2 us
  *   DMA 阻塞（start 后立即 wait）    7.69 us   ← 1.31 启动 + 4.19 线上 + 2.04 收尾
  *   DMA 异步（start + 干活 + wait）  3.35 us CPU，线上时间被完全掩盖（占周期 6.7%）
  *
  * 两个反直觉的结论：
  *   1. **DMA 比寄存器轮询快**。SPI3 挂在 APB1(42 MHz) 上，逐字节轮询 SPI3->SR
  *      每次都要付 AHB-APB 桥延迟；而 DMA 流寄存器和 DMA1->LISR 都在 AHB1 上，
  *      几乎零开销。
  *   2. **真正的瓶颈是 APB1 访问次数**。把热路径上的 APB1 访问从 6 次减到 0 次
  *      （收尾不读 SR、start 不改 CR1/CR2、SPE/DMAEN 一次性永久打开），
  *      单这一步就省下约 1.5 us。
  *
  * bsp_spi_transfer()（寄存器轮询）保留下来，作为 port 没提供 DMA 时的兜底，
  * 以及低速配置寄存器访问（对实时性无要求）的实现。
  *
  * 20 kHz 控制环推荐用 DMA 异步版，把 4.19 us 线上时间藏进 FOC 运算里：
  *
  *     mt6835_read_start(&enc);       // 1.3 us，SPI 开始在后台搬数据
  *     ... Clarke/Park/PI/SVPWM 运算 ...
  *     mt6835_read_finish(&enc);      // 2.0 us，数据早就到了
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
    BSP_SPI_ERR_BUSY,        /**< 上一次传输还没结束 */
    BSP_SPI_ERR_TIMEOUT,     /**< 等待标志位超时 */
    BSP_SPI_ERR_OVERRUN,     /**< 接收溢出：数据没被及时取走 */
    BSP_SPI_ERR_MODE,        /**< 模式/标志位错误（含 MODF/FRE） */
    BSP_SPI_ERR_DMA,         /**< DMA 传输错误 */
    BSP_SPI_ERR_OTHER
} bsp_spi_err_t;

/** @brief 一条 SPI 总线。 */
typedef struct {
    void    *hspi;        /**< 实际类型 SPI_HandleTypeDef* */
    uint32_t timeout_ms;  /**< 兼容字段；寄存器级实现改用内部循环计数保护 */
    bool     dma_ready;   /**< bsp_spi_dma_init() 是否成功 */
    bool     dma_busy;    /**< 是否有 DMA 传输在进行中 */
    bool     dma_need_flush; /**< 上次传输可能残留 RXNE/OVR，下次 start 前需清 */
} bsp_spi_t;

/**
  * @brief  绑定一条已由 CubeMX 初始化好的 SPI 总线。
  * @param  hspi        SPI_HandleTypeDef*（以 void* 传入以隔离 HAL 类型）
  * @param  timeout_ms  故障保护时长（仅用于 DMA 等待）
  */
void bsp_spi_bind(bsp_spi_t *bus, void *hspi, uint32_t timeout_ms);

/* -------------------------------------------------------------------------- */
/* 阻塞路径                                                                    */
/* -------------------------------------------------------------------------- */

/**
  * @brief  全双工收发 len 个字节（阻塞，寄存器级实现）。
  * @param  tx  发送缓冲，len 字节
  * @param  rx  接收缓冲，len 字节
  * @note   SPI 是全双工：想读 N 字节就必须同时写 N 字节，写 0x00 即可。
  *         本函数不操作片选，片选由调用方（驱动层）控制，这样才能保证
  *         整个帧在一次片选低电平内完成。
  */
bsp_spi_err_t bsp_spi_transfer(bsp_spi_t *bus,
                               const uint8_t *tx,
                               uint8_t       *rx,
                               uint16_t       len);

/* -------------------------------------------------------------------------- */
/* DMA 异步路径                                                                */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化 DMA 通道（把 SPI 的收发绑到 DMA 流上）。
  * @retval true  DMA 可用，随后可以走 start/wait
  * @note   只需调用一次，通常在板级 port 绑定时。不依赖 CubeMX 的 DMA 配置，
  *         全部在本层用寄存器配置，因此改 .ioc 重新生成代码不会影响它。
  */
bool bsp_spi_dma_init(bsp_spi_t *bus);

/** @brief DMA 是否已就绪。 */
bool bsp_spi_dma_available(const bsp_spi_t *bus);

/**
  * @brief  发起一次 DMA 收发，立即返回。
  * @note   调用方负责片选：**必须在调用前拉低片选，并在 wait 返回后才释放**，
  *         因为 DMA 期间片选必须一直保持有效。
  */
bsp_spi_err_t bsp_spi_transfer_dma_start(bsp_spi_t *bus,
                                         const uint8_t *tx,
                                         uint8_t       *rx,
                                         uint16_t       len);

/**
  * @brief  等待 DMA 收发完成。
  * @param  timeout_us 超时（微秒）
  * @note   若 start 与 wait 之间做了别的工作（例如 FOC 运算），
  *         线上时间就被完全藏起来了，本函数几乎立即返回。
  */
bsp_spi_err_t bsp_spi_transfer_dma_wait(bsp_spi_t *bus, uint32_t timeout_us);

/** @brief 是否有 DMA 传输正在进行。 */
bool bsp_spi_dma_busy(const bsp_spi_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_BSP_BSP_SPI_H */
