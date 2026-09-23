#ifndef TEST_USB_STUB_H
#define TEST_USB_STUB_H
#include <stdint.h>
#define USBD_OK 0u
#define USBD_BUSY 1u
#define USBD_STATE_CONFIGURED 3u
#define OTG_FS_IRQn 67
#define CDC_IN_EP 0x81u
#define __DMB() ((void)0)
typedef struct { uint32_t TxState; } USBD_CDC_HandleTypeDef;
typedef struct { uint8_t dev_state; void *pClassData; } USBD_HandleTypeDef;
void NVIC_DisableIRQ(int irq);
void NVIC_EnableIRQ(int irq);
uint32_t HAL_GetTick(void);
uint8_t CDC_Transmit_FS(uint8_t *data, uint16_t size);
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device);
uint8_t USBD_LL_FlushEP(USBD_HandleTypeDef *device, uint8_t ep);
#endif
