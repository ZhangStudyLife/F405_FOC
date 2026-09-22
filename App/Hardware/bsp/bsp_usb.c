#include "bsp_usb.h"
#include "usbd_cdc_if.h"
#include <string.h>

/* CPU-only OTG FS can read CCM; leave DMA-accessible SRAM to acquisition.
   No initialization needed: only published bytes are ever transmitted. */
static uint8_t s_tx[65536] __attribute__((section(".usb_buffer"), aligned(4)));
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_sending;
static uint32_t s_sent_at;
static volatile bool s_open;
static uint8_t *s_rx;
static uint32_t s_rx_size, s_rx_used;
static volatile bool s_rx_pending;
volatile bsp_usb_stats_t g_usb_stats;
extern USBD_HandleTypeDef hUsbDeviceFS;

bool bsp_usb_ready(void)
{
    return s_open && !g_usb_stats.overflow &&
           hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED;
}

bool bsp_usb_write(const void *data, size_t size)
{
    if (!data || !size || size > sizeof s_tx || !bsp_usb_ready()) return false;
    uint32_t head = s_head, used = head - s_tail;
    if (size > sizeof s_tx - used) {
        ++g_usb_stats.tx_rejected;
        g_usb_stats.overflow = true;
        return false;
    }
    uint32_t offset = head & (sizeof s_tx - 1u);
    size_t first = sizeof s_tx - offset;
    if (first > size) first = size;
    memcpy(s_tx + offset, data, first);
    if (size > first) memcpy(s_tx, (const uint8_t *)data + first, size - first);
    __DMB(); /* Publish only after the complete frame is copied. */
    s_head = head + (uint32_t)size;
    if (used + size > g_usb_stats.tx_high_water) g_usb_stats.tx_high_water = used + size;
    return true;
}

/* Called with USB IRQ excluded or from that IRQ. No global interrupt mask. */
static void transmit(void)
{
    if (s_sending || hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return;
    uint32_t size = s_head - s_tail;
    __DMB();
    if (!size) return;
    uint32_t now = HAL_GetTick();
    if (size < 4096u && now - s_sent_at < 10u && !g_usb_stats.overflow) return;
    uint32_t offset = s_tail & (sizeof s_tx - 1u);
    if (size > sizeof s_tx - offset) size = sizeof s_tx - offset;
    if (size > 4096u) size = 4096u;
    /* Keep full USB packets while streaming; the 10 ms flush sends the remainder. */
    if (size >= 64u) size &= ~63u;
    s_sending = size;
    if (CDC_Transmit_FS(s_tx + offset, (uint16_t)size) != USBD_OK) s_sending = 0u;
    else s_sent_at = now;
}

void bsp_usb_transmitted(void)
{
    g_usb_stats.tx_bytes += s_sending;
    __DMB(); /* Buffer remains owned by USB through the final packet/ZLP. */
    s_tail += s_sending;
    s_sending = 0u;
    transmit();
}

void bsp_usb_poll(void)
{
    if ((s_sending || s_head == s_tail) && !s_rx_pending) return;
    NVIC_DisableIRQ(OTG_FS_IRQn);
    transmit();
    if (s_rx_pending && s_rx_used == s_rx_size &&
        hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
        if (USBD_CDC_ReceivePacket(&hUsbDeviceFS) == USBD_OK) s_rx_pending = false;
    }
    NVIC_EnableIRQ(OTG_FS_IRQn);
}

void bsp_usb_reset(void)
{
    s_open = false;
    __DMB();
    g_usb_stats.tx_discarded += s_head - s_tail;
    s_head = s_tail = s_sending = 0u;
    s_rx_size = s_rx_used = 0u;
    s_rx_pending = false;
    g_usb_stats.overflow = false;
}

void bsp_usb_control(bool open)
{
    if (open && !s_open) ++g_usb_stats.sessions;
    if (!open && s_open) ++g_usb_stats.interruptions;
    s_open = open;
}

void bsp_usb_suspend(void)
{
    if (s_open) {
        ++g_usb_stats.interruptions;
        g_usb_stats.overflow = true; /* A suspended host cannot guarantee continuity. */
    }
}

void bsp_usb_received(uint8_t *data, uint32_t size)
{
    s_rx = data;
    s_rx_used = 0u;
    s_rx_size = size;
    s_rx_pending = true; /* Leave OUT NAKed until the foreground consumes it. */
    g_usb_stats.rx_bytes += size;
}

size_t bsp_usb_read(void *data, size_t size)
{
    if (!data || !size) return 0u;
    NVIC_DisableIRQ(OTG_FS_IRQn);
    size_t available = s_rx_size - s_rx_used;
    if (size > available) size = available;
    if (size) memcpy(data, s_rx + s_rx_used, size);
    s_rx_used += size;
    NVIC_EnableIRQ(OTG_FS_IRQn);
    return size;
}
