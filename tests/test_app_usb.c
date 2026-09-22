/* Exercise the real app parser and telemetry with the real current controller. */
#include "app.h"
#include "bsp_adc.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "bsp_usb.h"
#include "mt6835_port_stm32.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

volatile bsp_uart_stats_t g_uart_stats;
volatile bsp_usb_stats_t g_usb_stats;
volatile bsp_adc_sample_t adc_sample = {1.652f, 1.648f, 24.0f};
volatile float mt6835_angle_deg = 20.0f, mt6835_sample_delay = 2e-6f, motor_duty[3];
volatile unsigned motor_mode;
volatile uint32_t motor_sample_us;
extern volatile uint32_t app_command_rejected;
static const char *uart_input, *usb_input;
static size_t uart_left, usb_left;
static unsigned frames;
static float latest[9];

uint32_t bsp_motor_lock(void) { return 0u; }
void bsp_motor_unlock(uint32_t key) { (void)key; }
void bsp_motor_arm(void) {}
void bsp_motor_off(void) { motor_mode = MOTOR_OFF; }
bool bsp_motor_load(foc_calibration_t *cal) { *cal = (foc_calibration_t){0.0f, 1}; return true; }
bool bsp_motor_save(const foc_calibration_t *cal) { (void)cal; return true; }
bool bsp_motor_write(const float duty[3], unsigned mode) { (void)duty; (void)mode; return true; }
void bsp_motor_sample_end(void) {}
void bsp_adc_start(void) {}
void bsp_adc_stop(void) {}
bool bsp_uart_init(void) { return true; }
bool bsp_can_init(void) { return true; }
bool mt6835_init(void) { return true; }
void mt6835_stop(void) {}
uint32_t bsp_uart_millis(void) { return 0u; }
void bsp_uart_tick(void) {}
bool bsp_uart_write(const void *data, size_t size) { (void)data; assert(size == 64u); return true; }
bool bsp_usb_ready(void) { return true; }
void bsp_usb_poll(void) {}
bool bsp_usb_write(const void *data, size_t size)
{
    assert(size == sizeof latest);
    memcpy(latest, data, size); ++frames; return true;
}
static size_t read_input(void *data, size_t size, const char **input, size_t *left)
{
    if (size > *left) size = *left;
    if (size) { memcpy(data, *input, size); *input += size; *left -= size; }
    return size;
}
size_t bsp_uart_read(void *data, size_t size) { return read_input(data, size, &uart_input, &uart_left); }
size_t bsp_usb_read(void *data, size_t size) { return read_input(data, size, &usb_input, &usb_left); }
static void usb(const char *text)
{
    usb_input = text; usb_left = strlen(text);
    do { app_poll(); } while (usb_left);
}
static void uart(const char *text)
{
    uart_input = text; uart_left = strlen(text); app_poll();
}
static void running(void)
{
    foc.state = FOC_RUN; foc.zero_ready = true;
    foc.b_offset = foc.c_offset = 1.65f;
}

int main(void)
{
    assert(app_init()); running();
    usb("Iq 0."); assert(foc.command == 0.0f);
    usb("25\r"); assert(foc.command == 0.25f);
    usb("\n"); assert(app_command_rejected == 0u);
    usb("Iq -0.30\r\nIq 0.40\n"); assert(foc.command == 0.4f);
    /* Each transport owns its partial line. */
    usb("Iq 0."); uart("Iq -0.20\r");
    assert(foc.command == -0.2f);
    usb("35\r"); assert(foc.command == 0.35f);
    const char *bad[] = {"Iq nan\r", "Iq inf\r", "Iq 5.01\r", "Iq -5.01\r",
        "Iq 0.123\r", "Iq 1e0\r", "Iq .5\r", "Iq 1.\r", "Iq 0.2junk\r",
        "Iq 0000000000000000000000000000000000001\r", "Iq 0.\00120\r"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        unsigned rejected = app_command_rejected;
        usb(bad[i]);
        assert(app_command_rejected == rejected + 1u && foc.command == 0.35f);
    }
    usb("Iq 0."); ++g_usb_stats.sessions; usb("45\r");
    assert(foc.command == 0.35f); /* Old session prefix cannot start a command. */
    usb("Iq 0.50\r"); assert(foc.command == 0.5f);
    for (unsigned n = 0; n < 20000u; ++n) {
        motor_sample_us = (0xffff00u + n * 50u) & 0xffffffu;
        app_sample();
        assert(latest[0] == (float)motor_sample_us);
        assert(latest[1] == foc.command);
        assert(fabsf(latest[2] - 0.1f) < 1e-5f && fabsf(latest[3] + 0.1f) < 1e-5f);
        assert(latest[4] == foc.id && latest[5] == foc.iq && latest[6] == foc.uq);
        assert(latest[7] == mt6835_angle_deg && isinf(latest[8]) && latest[8] > 0);
    }
    assert(frames == 20000u && foc.state == FOC_RUN);
    usb("stop\r"); assert(foc.state == FOC_IDLE && foc.command == 0.0f && motor_mode == MOTOR_OFF);
    usb("Iq 0.00\r"); assert(foc.state == FOC_IDLE);
    usb("Iq 0.20\r"); assert(foc.state == FOC_PRECHARGE && foc.command == 0.2f);
    usb("stop\r");
    foc.zero_ready = false; app_sample(); assert(isnan(latest[2]) && isnan(latest[3]));
    puts("PASS: USB CR/LF/CRLF, split/coalesced commands, UART isolation, invalid/long lines, sessions, 20k current frames");
}
