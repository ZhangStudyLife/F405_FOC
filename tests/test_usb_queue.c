/* Exercise the production queue with a deliberately delayed USB consumer. */
#include <assert.h>
#include <stdio.h>
#include "../App/Hardware/bsp/bsp_usb.c"

static USBD_CDC_HandleTypeDef cdc;
USBD_HandleTypeDef hUsbDeviceFS = {USBD_STATE_CONFIGURED, &cdc};
static uint32_t tick, rearms;
static uint8_t *inflight;
static uint16_t inflight_size;
static bool busy;
static uint8_t expected[720000];
static size_t produced, consumed;
void NVIC_DisableIRQ(int irq) { (void)irq; }
void NVIC_EnableIRQ(int irq) { (void)irq; }
uint32_t HAL_GetTick(void) { return tick; }
uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device)
{
    (void)device; ++rearms; return USBD_OK;
}
uint8_t USBD_LL_FlushEP(USBD_HandleTypeDef *device, uint8_t ep)
{
    (void)device; (void)ep; inflight = NULL; return USBD_OK;
}
uint8_t CDC_Transmit_FS(uint8_t *data, uint16_t size)
{
    if (busy) return USBD_BUSY;
    assert(!inflight && size && size <= 16384u);
    inflight = data; inflight_size = size;
    return USBD_OK;
}
static void complete(void)
{
    if (!inflight) return;
    assert(consumed + inflight_size <= produced);
    assert(memcmp(inflight, expected + consumed, inflight_size) == 0);
    consumed += inflight_size;
    inflight = NULL;
    bsp_usb_transmitted();
}
static void enqueue(unsigned n)
{
    uint8_t frame[36];
    for (unsigned i = 0; i < sizeof frame; ++i) frame[i] = (uint8_t)(n + i);
    assert(bsp_usb_write(frame, sizeof frame));
    memcpy(expected + produced, frame, sizeof frame);
    produced += sizeof frame;
    memset(frame, 0xa5, sizeof frame); /* Caller storage may be reused immediately. */
}
int main(void)
{
    assert(!bsp_usb_ready());
    bsp_usb_reset(); bsp_usb_control(true);
    /* Start near counter rollover; force frame and transfer wraparound too. */
    s_head = s_tail = UINT32_MAX - 15u;
    for (unsigned n = 0; n < 20000u; ++n) {
        enqueue(n);
        tick = n / 20u;
        busy = n % 137u < 5u;
        bsp_usb_poll();
        if (n % 61u == 0u) complete();
    }
    busy = false;
    while (consumed < produced) { tick += 2; bsp_usb_poll(); complete(); }
    assert(!g_usb_stats.overflow && g_usb_stats.tx_bytes == produced);

    bsp_usb_reset(); bsp_usb_control(true);
    produced = consumed = 0;
    /* A stalled host must never allow the producer to overwrite inflight data. */
    for (unsigned n = 0; n < sizeof s_tx / 36u; ++n) { enqueue(n); bsp_usb_poll(); }
    uint8_t frame[36] = {0};
    assert(!bsp_usb_write(frame, sizeof frame));
    assert(g_usb_stats.overflow && g_usb_stats.tx_rejected == 1u);
    assert(!bsp_usb_ready());
    while (consumed < produced) { tick += 2; bsp_usb_poll(); complete(); }
    assert(!bsp_usb_write(frame, sizeof frame)); /* Fault remains latched after drain. */
    bsp_usb_reset(); bsp_usb_control(true);
    assert(bsp_usb_ready());

    uint8_t rx[] = {1, 2, 3, 4}, out[4];
    bsp_usb_received(rx, sizeof rx);
    assert(bsp_usb_read(out, 2) == 2);
    bsp_usb_poll(); assert(rearms == 0);
    assert(bsp_usb_read(out + 2, 4) == 2);
    assert(memcmp(rx, out, 4) == 0);
    bsp_usb_poll(); assert(rearms == 1);
    bsp_usb_poll(); assert(rearms == 1);
    bsp_usb_received(rx, 0); bsp_usb_poll(); assert(rearms == 2);

    bsp_usb_suspend(); assert(!bsp_usb_ready() && g_usb_stats.interruptions == 1);
    bsp_usb_reset(); bsp_usb_control(true);
    enqueue(0); bsp_usb_reset();
    assert(g_usb_stats.tx_discarded == 36u && !bsp_usb_ready());
    puts("PASS: 20k frames, BUSY retry, ownership, ring/counter wrap, overflow latch, reset, RX backpressure");
}
