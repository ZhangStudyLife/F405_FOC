#ifndef TEST_USB_STUB_H
#define TEST_USB_STUB_H
#include <stdint.h>
#define USBD_OK 0u
#define USBD_BUSY 1u
#define USBD_STATE_CONFIGURED 3u
#define OTG_FS_IRQn 67
#define __DMB() ((void)0)
typedef struct { uint8_t dev_state; } USBD_HandleTypeDef;
void NVIC_DisableIRQ(int irq);
void NVIC_EnableIRQ(int irq);
uint32_t HAL_GetTick(void);
uint8_t CDC_Transmit_FS(uint8_t *data, uint16_t size);
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device);
#endif
