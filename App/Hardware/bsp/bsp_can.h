#ifndef APP_HARDWARE_BSP_CAN_H
#define APP_HARDWARE_BSP_CAN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    bool ext, rtr;
    uint8_t dlc, data[8];
} bsp_can_frame_t;

extern volatile uint32_t can_rx_lost;

/* CAN1, 1 Mbps from CubeMX. Send/receive never wait for the bus. */
bool bsp_can_init(void);
bool bsp_can_send(const bsp_can_frame_t *frame);
bool bsp_can_recv(bsp_can_frame_t *frame);

#endif
