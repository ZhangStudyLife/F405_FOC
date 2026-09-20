#include "bsp_can.h"
#include "can.h"

static bsp_can_frame_t s_rx[16];
static volatile uint8_t s_head, s_tail;
volatile uint32_t can_rx_lost;

bool bsp_can_init(void)
{
    CAN_FilterTypeDef filter = {
        .FilterMode = CAN_FILTERMODE_IDMASK,
        .FilterScale = CAN_FILTERSCALE_32BIT,
        .FilterFIFOAssignment = CAN_FILTER_FIFO0,
        .FilterActivation = ENABLE,
        .SlaveStartFilterBank = 14u
    };
    return HAL_CAN_ConfigFilter(&hcan1, &filter) == HAL_OK &&
           HAL_CAN_Start(&hcan1) == HAL_OK &&
           HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) == HAL_OK;
}

bool bsp_can_send(const bsp_can_frame_t *frame)
{
    if (frame->dlc > 8u || frame->id > (frame->ext ? 0x1fffffffu : 0x7ffu)) return false;
    CAN_TxHeaderTypeDef tx = {
        .StdId = frame->id, .ExtId = frame->id,
        .IDE = frame->ext ? CAN_ID_EXT : CAN_ID_STD,
        .RTR = frame->rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA,
        .DLC = frame->dlc
    };
    uint32_t mailbox;
    return HAL_CAN_AddTxMessage(&hcan1, &tx, frame->data, &mailbox) == HAL_OK;
}

bool bsp_can_recv(bsp_can_frame_t *frame)
{
    if (s_tail == s_head) return false;
    *frame = s_rx[s_tail];
    s_tail = (s_tail + 1u) & 15u;
    return true;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *can)
{
    CAN_RxHeaderTypeDef rx;
    bsp_can_frame_t frame;
    while (HAL_CAN_GetRxFifoFillLevel(can, CAN_RX_FIFO0)) {
        if (HAL_CAN_GetRxMessage(can, CAN_RX_FIFO0, &rx, frame.data) != HAL_OK) break;
        frame.id = rx.IDE == CAN_ID_EXT ? rx.ExtId : rx.StdId;
        frame.ext = rx.IDE == CAN_ID_EXT;
        frame.rtr = rx.RTR == CAN_RTR_REMOTE;
        frame.dlc = (uint8_t)rx.DLC;
        uint8_t next = (s_head + 1u) & 15u;
        if (next == s_tail) can_rx_lost++;
        else {
            s_rx[s_head] = frame;
            s_head = next;
        }
    }
}
