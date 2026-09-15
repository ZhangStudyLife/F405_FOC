/**
  ******************************************************************************
  * @file    bsp_can.c
  * @brief   CAN 总线抽象实现（STM32F4 bxCAN / HAL）。
  *
  * 本文件是整个 App 层里少数允许 include HAL 的文件之一（见 App/README.md）。
  *
  * 分层：本层只做"外设 + 队列"，不做协议解析、不定义报文语义。
  * 上层（应用/协议层）拿到 bsp_can_frame_t 后自己解释 ID 和数据含义。
  ******************************************************************************
  */

#include "bsp_can.h"

#include "stm32f4xx_hal.h"

/* -------------------------------------------------------------------------- */
/* 实例登记：中断回调只有 hcan 指针，靠这张表反查回 bsp_can_t                    */
/* -------------------------------------------------------------------------- */

static bsp_can_t *s_can_owner[2];   /* [0]=CAN1, [1]=CAN2 */

static int can_slot(const CAN_HandleTypeDef *hcan)
{
    if (hcan->Instance == CAN1) {
        return 0;
    }
#if defined(CAN2)
    if (hcan->Instance == CAN2) {
        return 1;
    }
#endif
    return -1;
}

/* -------------------------------------------------------------------------- */
/* 启动序列                                                                    */
/* -------------------------------------------------------------------------- */

/**
  * @brief  配置过滤器 → 启动 → 打开 RX FIFO0 中断通知。
  * @note   过滤器用"全通过"：ID 和掩码全 0，任何报文都接收并落到 FIFO0。
  *         这是打通链路阶段的合理默认；以后要按 ID 过滤，改 FilterId/Mask 即可，
  *         不需要动队列和回调。
  */
static bsp_can_err_t can_configure_and_start(bsp_can_t *can, CAN_HandleTypeDef *hcan)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterBank           = 0u;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = 0x0000u;
    filter.FilterIdLow          = 0x0000u;
    filter.FilterMaskIdHigh     = 0x0000u;
    filter.FilterMaskIdLow      = 0x0000u;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation     = ENABLE;
    filter.SlaveStartFilterBank = 14u;   /* 只用 CAN1 时该字段无影响 */

    if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK) {
        return BSP_CAN_ERR_START;
    }
    if (HAL_CAN_Start(hcan) != HAL_OK) {
        return BSP_CAN_ERR_START;
    }
    if (HAL_CAN_ActivateNotification(hcan, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
        return BSP_CAN_ERR_START;
    }

    can->started = true;
    return BSP_CAN_OK;
}

bsp_can_err_t bsp_can_init(bsp_can_t *can, void *hcan)
{
    CAN_HandleTypeDef *h;
    int                slot;

    if ((can == NULL) || (hcan == NULL)) {
        return BSP_CAN_ERR_NOT_BOUND;
    }

    h    = (CAN_HandleTypeDef *)hcan;
    slot = can_slot(h);
    if (slot < 0) {
        return BSP_CAN_ERR_NOT_BOUND;
    }

    /* 允许重复调用：先停掉可能正在跑的外设和通知 */
    if (HAL_CAN_GetState(h) == HAL_CAN_STATE_LISTENING) {
        (void)HAL_CAN_DeactivateNotification(h, CAN_IT_RX_FIFO0_MSG_PENDING);
        (void)HAL_CAN_Stop(h);
    }

    can->hcan     = hcan;
    can->rx_head  = 0u;
    can->rx_tail  = 0u;
    can->rx_total = 0u;
    can->rx_lost  = 0u;
    can->tx_total = 0u;
    can->started  = false;
    can->mode     = BSP_CAN_MODE_NORMAL;

    s_can_owner[slot] = can;

    return can_configure_and_start(can, h);
}

/* -------------------------------------------------------------------------- */
/* 发送 / 接收                                                                  */
/* -------------------------------------------------------------------------- */

bsp_can_err_t bsp_can_send(bsp_can_t *can, const bsp_can_frame_t *f)
{
    CAN_HandleTypeDef  *h;
    CAN_TxHeaderTypeDef tx;
    uint32_t            mailbox;

    if ((can == NULL) || (can->hcan == NULL) || (f == NULL)) {
        return BSP_CAN_ERR_NOT_BOUND;
    }
    if (f->dlc > 8u) {
        return BSP_CAN_ERR_PARAM;
    }
    if (f->ext ? (f->id > 0x1FFFFFFFu) : (f->id > 0x7FFu)) {
        return BSP_CAN_ERR_PARAM;
    }

    h = (CAN_HandleTypeDef *)can->hcan;

    tx.StdId              = f->ext ? 0u : f->id;
    tx.ExtId              = f->ext ? f->id : 0u;
    tx.IDE                = f->ext ? CAN_ID_EXT : CAN_ID_STD;
    tx.RTR                = f->rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    tx.DLC                = f->dlc;          /* HAL 期望的就是字节数 0~8 */
    tx.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(h, &tx, f->data, &mailbox) != HAL_OK) {
        return BSP_CAN_ERR_TX_FULL;
    }

    can->tx_total++;
    return BSP_CAN_OK;
}

bool bsp_can_recv(bsp_can_t *can, bsp_can_frame_t *f)
{
    if ((can == NULL) || (f == NULL)) {
        return false;
    }
    if (can->rx_tail == can->rx_head) {
        return false;                       /* 队列空 */
    }

    *f = can->rxq[can->rx_tail];
    can->rx_tail = (uint16_t)((can->rx_tail + 1u) % BSP_CAN_RX_QUEUE_LEN);
    return true;
}

/**
  * @brief  RX FIFO0 有新报文：搬进软件队列。
  * @note   在中断上下文执行，只做搬运和计数，不解析、不打印。
  *
  *   必须**把 FIFO 读空再返回**。bxCAN 的 FMPIE0 是电平触发的：只要
  *   RF0R.FMP0 != 0，HAL_CAN_IRQHandler 就会反复进来。留一帧不读，轻则
  *   下次立刻再次中断，重则 FIFO 溢出丢帧（硬件置 OVR0）。
  *
  *   队列满时丢**新**帧而不是覆盖旧帧，但帧已经从 FIFO 取走，所以总线侧
  *   不会因此堵住 —— 只是这份数据没进队列，由 rx_lost 暴露出来。
  */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    bsp_can_t          *can;
    CAN_RxHeaderTypeDef rx;
    bsp_can_frame_t     frame;
    uint16_t            next;
    int                 slot;

    slot = can_slot(hcan);
    if (slot < 0) {
        return;
    }
    can = s_can_owner[slot];
    if (can == NULL) {
        return;
    }

    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0u) {
        if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx, frame.data) != HAL_OK) {
            break;   /* 理论上进不来；真发生了也不要在这里空转 */
        }

        frame.id  = (rx.IDE == CAN_ID_EXT) ? rx.ExtId : rx.StdId;
        frame.ext = (rx.IDE == CAN_ID_EXT);
        frame.rtr = (rx.RTR == CAN_RTR_REMOTE);
        frame.dlc = (uint8_t)rx.DLC;

        next = (uint16_t)((can->rx_head + 1u) % BSP_CAN_RX_QUEUE_LEN);
        if (next == can->rx_tail) {
            can->rx_lost++;
            continue;            /* 已从 FIFO 取走，继续读空 */
        }

        can->rxq[can->rx_head] = frame;
        can->rx_head = next;
        can->rx_total++;
    }
}

/* -------------------------------------------------------------------------- */
/* 模式切换与自测                                                                */
/* -------------------------------------------------------------------------- */

bsp_can_err_t bsp_can_set_mode(bsp_can_t *can, bsp_can_mode_t mode)
{
    CAN_HandleTypeDef *h;
    uint32_t           hal_mode;

    if ((can == NULL) || (can->hcan == NULL)) {
        return BSP_CAN_ERR_NOT_BOUND;
    }
    if (mode > BSP_CAN_MODE_SILENT_LOOPBACK) {
        return BSP_CAN_ERR_PARAM;
    }

    h = (CAN_HandleTypeDef *)can->hcan;

    switch (mode) {
        case BSP_CAN_MODE_LOOPBACK:        hal_mode = CAN_MODE_LOOPBACK;        break;
        case BSP_CAN_MODE_SILENT_LOOPBACK: hal_mode = CAN_MODE_SILENT_LOOPBACK; break;
        case BSP_CAN_MODE_NORMAL:
        default:                           hal_mode = CAN_MODE_NORMAL;          break;
    }

    if (HAL_CAN_GetState(h) == HAL_CAN_STATE_LISTENING) {
        (void)HAL_CAN_DeactivateNotification(h, CAN_IT_RX_FIFO0_MSG_PENDING);
        (void)HAL_CAN_Stop(h);
    }
    can->started = false;

    /* HAL_CAN_Init 会复位外设，因此过滤器与通知必须在它之后再建一次 */
    h->Init.Mode = hal_mode;
    if (HAL_CAN_Init(h) != HAL_OK) {
        return BSP_CAN_ERR_OTHER;
    }

    can->mode = mode;
    return can_configure_and_start(can, h);
}

bool bsp_can_in_loopback(const bsp_can_t *can)
{
    if (can == NULL) {
        return false;
    }
    return (can->mode == BSP_CAN_MODE_LOOPBACK) ||
           (can->mode == BSP_CAN_MODE_SILENT_LOOPBACK);
}

bool bsp_can_selftest(bsp_can_t *can, bsp_can_frame_t *out_rx)
{
    bsp_can_frame_t tx;
    bsp_can_frame_t rx;
    bsp_can_mode_t  saved;
    uint32_t        t0;
    uint8_t         i;
    bool            ok = false;

    if ((can == NULL) || (can->hcan == NULL)) {
        return false;
    }

    saved = can->mode;
    if (bsp_can_set_mode(can, BSP_CAN_MODE_LOOPBACK) != BSP_CAN_OK) {
        return false;
    }

    /* 先丢掉切换前残留的帧，免得把旧数据当成自己的回声 */
    can->rx_tail = can->rx_head;

    tx.id  = 0x7A5u;
    tx.ext = false;
    tx.rtr = false;
    tx.dlc = 8u;
    for (i = 0u; i < 8u; i++) {
        tx.data[i] = (uint8_t)(0xA0u + i);
    }

    if (bsp_can_send(can, &tx) != BSP_CAN_OK) {
        goto restore;
    }

    /* 轮询等回声。回环模式下报文不出控制器，通常几十微秒内就到 FIFO，
       100 ms 只是防呆上限，正常不会走到。 */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 100u) {
        if (bsp_can_recv(can, &rx)) {
            ok = (rx.id == tx.id) && (rx.dlc == tx.dlc) && (!rx.ext);
            if (ok && (out_rx != NULL)) {
                *out_rx = rx;
            }
            break;
        }
    }

restore:
    (void)bsp_can_set_mode(can, saved);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* 统计                                                                        */
/* -------------------------------------------------------------------------- */

uint32_t bsp_can_rx_count(const bsp_can_t *can)
{
    return (can == NULL) ? 0u : can->rx_total;
}

uint32_t bsp_can_rx_lost(const bsp_can_t *can)
{
    return (can == NULL) ? 0u : can->rx_lost;
}

uint32_t bsp_can_tx_count(const bsp_can_t *can)
{
    return (can == NULL) ? 0u : can->tx_total;
}
