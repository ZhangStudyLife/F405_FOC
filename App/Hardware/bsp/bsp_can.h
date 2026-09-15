/**
  ******************************************************************************
  * @file    bsp_can.h
  * @brief   CAN 总线抽象：帧收发 + 中断接收队列 + 回环自测。
  *
  * 设计要点：
  *   - 头文件里没有 HAL 类型：句柄用 void* 承载，真实类型是 CAN_HandleTypeDef*。
  *   - 错误码是本层定义的枚举，不透传 HAL_StatusTypeDef。
  *   - **位时序不在本层决定**：由 CubeMX 在 MX_CAN1_Init() 里设好。当前配置
  *     APB1 42 MHz / Prescaler 3 / BS1 8TQ / BS2 5TQ / SJW 2TQ
  *     → 42e6 / (3 x (1+8+5)) = **1 Mbps**，采样点 (1+8)/14 = 64.3%。
  *     本层只负责过滤器、启动、中断通知和队列。
  *   - 接收路径是"中断入队 + 非阻塞出队"：控制环不需要轮询外设寄存器，
  *     也不需要在 ISR 里做解析。队列满了丢新帧并计数，不阻塞、不覆盖旧数据。
  *
  * ---------------------------------------------------------------------------
  * 关于"回环"（Loopback）—— 它不是终端电阻
  * ---------------------------------------------------------------------------
  *   板子上 CAN_H/CAN_L 之间那颗 120Ω 是**终端匹配电阻**，吸收信号反射，
  *   和回环没有任何关系。
  *
  *   回环是 bxCAN 控制器的一个**测试模式**，三种模式的区别：
  *
  *     NORMAL          正常收发，TXD 输出到收发器，RXD 来自总线
  *     LOOPBACK        忽略 RXD 引脚输入，把**自己发出去的报文当作收到的报文**
  *                     存进接收 FIFO；TXD 仍然在输出波形。
  *     SILENT_LOOPBACK 同上，但 TXD 保持隐性不发 —— 总线上完全看不见这个节点，
  *                     适合在一个已经在跑的 CAN 网络旁边做离线自测。
  *
  *   为什么需要它：**单节点在 NORMAL 模式下发报文必然失败**。总线上没有其他
  *   节点给出 ACK 位，发送方会判定 ACK Error，错误计数器一路累加，最终进入
  *   Bus-Off。所以"自己发自己收"的验证必须走 LOOPBACK。
  *
  *   bsp_can_selftest() 就是干这件事：切到 LOOPBACK、发一帧、确认收到、
  *   再切回 NORMAL。它验证的是 过滤器 → 中断 → 队列 → 出队 这整条软件链路，
  *   **不经过收发器、不经过线缆**，因此接线是否正确它测不出来。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_BSP_BSP_CAN_H
#define APP_HARDWARE_BSP_BSP_CAN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 接收队列深度（帧数）。满了丢新帧并累加 lost 计数。 */
#ifndef BSP_CAN_RX_QUEUE_LEN
#define BSP_CAN_RX_QUEUE_LEN 16u
#endif

/** @brief CAN 收发结果。0 为成功，便于 `if (err)` 判断。 */
typedef enum {
    BSP_CAN_OK = 0,
    BSP_CAN_ERR_NOT_BOUND,   /**< 句柄为空或未绑定 */
    BSP_CAN_ERR_PARAM,       /**< 参数非法（DLC > 8、ID 越界等） */
    BSP_CAN_ERR_TX_FULL,     /**< 三个发送邮箱都满，本次未发出 */
    BSP_CAN_ERR_TX_FAIL,     /**< HAL 返回错误 */
    BSP_CAN_ERR_START,       /**< 过滤器/启动/中断通知失败 */
    BSP_CAN_ERR_OTHER
} bsp_can_err_t;

/** @brief CAN 控制器工作模式。 */
typedef enum {
    BSP_CAN_MODE_NORMAL = 0,  /**< 正常收发（接总线用这个） */
    BSP_CAN_MODE_LOOPBACK,    /**< 内回环：自发自收，TXD 仍输出 */
    BSP_CAN_MODE_SILENT_LOOPBACK /**< 静默内回环：自发自收，TXD 不发 */
} bsp_can_mode_t;

/** @brief 一帧经典 CAN 报文（CAN 2.0B，数据段最多 8 字节）。 */
typedef struct {
    uint32_t id;        /**< 11 位标准 ID（0x000~0x7FF）或 29 位扩展 ID */
    bool     ext;       /**< true = 29 位扩展帧，false = 11 位标准帧 */
    bool     rtr;       /**< 远程帧标志（一般用不到） */
    uint8_t  dlc;       /**< 数据长度 0~8 */
    uint8_t  data[8];   /**< 数据，只取前 dlc 字节 */
} bsp_can_frame_t;

/** @brief 一条 CAN 总线。 */
typedef struct {
    void    *hcan;                     /**< 实际类型 CAN_HandleTypeDef* */

    bsp_can_frame_t rxq[BSP_CAN_RX_QUEUE_LEN];
    volatile uint16_t rx_head;         /**< ISR 写 */
    volatile uint16_t rx_tail;         /**< 任务读 */
    volatile uint32_t rx_total;        /**< 累计入队帧数 */
    volatile uint32_t rx_lost;         /**< 因队列满而丢弃的帧数 */
    volatile uint32_t tx_total;        /**< 累计提交成功的发送帧数 */

    bsp_can_mode_t mode;               /**< 当前工作模式 */
    bool     started;                  /**< 是否已 Start */
} bsp_can_t;

/**
  * @brief  绑定一条已由 CubeMX 初始化好的 CAN 外设，并启动它。
  * @param  hcan  CAN_HandleTypeDef*（以 void* 传入以隔离 HAL 类型）
  * @note   做三件事：配置"全通过"过滤器到 FIFO0、使能 RX FIFO0 挂起中断、
  *         启动外设。位时序沿用 MX_CANx_Init() 里的设置，本函数不改。
  *         可重复调用（会先 Stop 再重新配置）。
  */
bsp_can_err_t bsp_can_init(bsp_can_t *can, void *hcan);

/**
  * @brief  提交一帧发送（非阻塞，不等待发送完成）。
  * @retval BSP_CAN_ERR_TX_FULL 三个邮箱都忙，调用方稍后重试即可。
  */
bsp_can_err_t bsp_can_send(bsp_can_t *can, const bsp_can_frame_t *f);

/**
  * @brief  从接收队列取一帧（非阻塞）。
  * @retval true  取到一帧
  * @retval false 队列为空
  */
bool bsp_can_recv(bsp_can_t *can, bsp_can_frame_t *f);

/**
  * @brief  切换工作模式（正常 / 回环 / 静默回环）。
  * @note   会 Stop → 改 Mode → HAL_CAN_Init → 重配过滤器 → Start。
  *         切换过程中队列内容保留。
  */
bsp_can_err_t bsp_can_set_mode(bsp_can_t *can, bsp_can_mode_t mode);

/** @brief 当前是否处于某个回环模式。 */
bool bsp_can_in_loopback(const bsp_can_t *can);

/**
  * @brief  回环自测：切到 LOOPBACK 发一帧并确认收回，然后恢复原模式。
  * @param  out_rx  非 NULL 时回填实际收到的帧，便于调用方打印比对
  * @retval true    发送与接收都成功，软件链路通
  * @note   测的是 过滤器→中断→队列→出队，**不经过收发器和线缆**。
  *         因此本函数通过 ≠ 接线正确。
  */
bool bsp_can_selftest(bsp_can_t *can, bsp_can_frame_t *out_rx);

/** @brief 累计收到的帧数。 */
uint32_t bsp_can_rx_count(const bsp_can_t *can);

/** @brief 因队列满丢弃的帧数。非 0 说明出队太慢。 */
uint32_t bsp_can_rx_lost(const bsp_can_t *can);

/** @brief 累计提交成功的发送帧数。 */
uint32_t bsp_can_tx_count(const bsp_can_t *can);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_BSP_BSP_CAN_H */
