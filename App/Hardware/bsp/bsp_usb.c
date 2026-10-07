#include "bsp_usb.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;
#define TX_SIZE 8192u
static uint8_t tx[TX_SIZE + 3u]; /* HAL reads the last FIFO word as four bytes. */
static uint8_t * volatile rx;
static volatile uint32_t head, tail, flight, rx_size, rx_used;
static volatile bool opened, discard;
volatile bsp_usb_stats_t g_usb_stats;

/* CubeMX has no USER CODE hook between its FIFO defaults and USB connect. */
HAL_StatusTypeDef __real_HAL_PCD_Start(PCD_HandleTypeDef *pcd);
HAL_StatusTypeDef __wrap_HAL_PCD_Start(PCD_HandleTypeDef *pcd)
{
    HAL_PCDEx_SetRxFiFo(pcd, 128);
    HAL_PCDEx_SetTxFiFo(pcd, 0, 32);
    HAL_PCDEx_SetTxFiFo(pcd, 1, 144);
    HAL_PCDEx_SetTxFiFo(pcd, 2, 16); /* RX + EP0 + data + notification = 320 words. */
    return __real_HAL_PCD_Start(pcd);
}

void bsp_usb_reset(void)
{
    opened = false;
    head = tail = flight = rx_size = rx_used = 0;
    rx = NULL;
    discard = false;
    ++g_usb_stats.session;
}

void bsp_usb_open(bool value)
{
    if (opened == value) return;
    discard = true; /* Gate the sample producer before changing this session. */
    opened = value;
    rx_used = rx_size;
    ++g_usb_stats.session;
}

void bsp_usb_suspend(void)
{
    discard = true;
    rx_used = rx_size;
    ++g_usb_stats.session;
}

void bsp_usb_received(uint8_t *data, uint32_t size)
{
    rx = data;
    rx_used = 0;
    rx_size = size;
}

void bsp_usb_transmitted(void)
{
    tail += flight; /* ST calls this after the last packet, including any ZLP. */
    __DMB();
    flight = 0;
}

bool bsp_usb_write(const void *data, size_t size)
{
    if (!data || !size || size > 256u || !opened || discard ||
        hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return false;
    uint32_t used = head - tail;
    if (size > TX_SIZE - used) { ++g_usb_stats.tx_rejected; return false; }
    size_t first = TX_SIZE - (head & (TX_SIZE - 1u));
    if (first > size) first = size;
    memcpy(tx + (head & (TX_SIZE - 1u)), data, first);
    memcpy(tx, (const uint8_t *)data + first, size - first);
    __DMB();
    head += size;
    if (used + size > g_usb_stats.tx_peak) g_usb_stats.tx_peak = used + size;
    return true;
}

size_t bsp_usb_read(void *data, size_t size)
{
    if (!data) return 0;
    NVIC_DisableIRQ(OTG_FS_IRQn);
    if (!opened || discard || hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) size = 0;
    if (size > rx_size - rx_used) size = rx_size - rx_used;
    if (size) { memcpy(data, rx + rx_used, size); rx_used += size; }
    NVIC_EnableIRQ(OTG_FS_IRQn);
    return size;
}

void bsp_usb_poll(void)
{
    NVIC_DisableIRQ(OTG_FS_IRQn); /* ADC/encoder/PWM IRQs remain enabled. */
    if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
        if (rx && (rx_used == rx_size || !opened || discard)) {
            rx = NULL;
            rx_size = rx_used = 0;
            USBD_CDC_ReceivePacket(&hUsbDeviceFS);
        }
        if (discard && !flight) {
            head = tail = 0;
            __DMB();
            discard = false;
        }
        if (opened && !discard && !flight && head != tail) {
            uint32_t size = TX_SIZE - (tail & (TX_SIZE - 1u));
            if (size > head - tail) size = head - tail;
            if (size > 256u) size = 256u;
            __DMB();
            flight = size;
            if (CDC_Transmit_FS(tx + (tail & (TX_SIZE - 1u)), size) != USBD_OK) flight = 0;
        }
    }
    NVIC_EnableIRQ(OTG_FS_IRQn);
}
