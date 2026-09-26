/* Exercise the real app parser and telemetry with the real current controller
   and the real 1 kHz outer-loop scheduler. */
#include "app.h"
#include "bsp_adc.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "bsp_usb.h"
#include "control.h"
#include "foc.h"
#include "mt6835_port_stm32.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

volatile bsp_uart_stats_t g_uart_stats;
volatile bsp_usb_stats_t g_usb_stats;
volatile bsp_adc_sample_t adc_sample = {1.652f, 1.648f, 24.0f};
volatile uint16_t adc_raw_b = 2050u, adc_raw_c = 2045u, adc_raw_bus = 1040u;
volatile float mt6835_angle_deg = 20.0f, mt6835_raw_deg = 20.0f, mt6835_sample_delay = 2e-6f;
volatile float motor_duty[3] = {0.4f, 0.5f, 0.6f};
volatile unsigned motor_mode;
volatile uint32_t motor_sample_us;
extern volatile uint32_t app_command_rejected;
static const char *uart_input, *usb_input;
static size_t uart_left, usb_left;
static unsigned frames;
/* The 15 float channels and the JustFloat terminator from app.c. */
static uint32_t latest[16];

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
static uint32_t s_millis;
uint32_t bsp_uart_millis(void) { return s_millis; }
void bsp_uart_tick(void) {}
static uint32_t s_uart_frames;
bool bsp_uart_write(const void *data, size_t size)
{
    (void)data;
    assert(size > 0u && size <= 256u); /* UART status telemetry or the hello banner. */
    if (size == 64u) ++s_uart_frames;
    return true;
}
bool bsp_usb_ready(void) { return true; }
void bsp_usb_poll(void) {}
bool bsp_usb_write(const void *data, size_t size)
{
    assert(size == 16u * sizeof(float));
    assert(isinf(((const float *)data)[15]));
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
static float channel(unsigned index)
{
    float value;
    memcpy(&value, &latest[index], sizeof value);
    return value;
}
static unsigned word(void)
{
    uint32_t value;
    memcpy(&value, &latest[0], sizeof value);
    return value;
}
static unsigned index_word(void)
{
    uint32_t value;
    memcpy(&value, &latest[1], sizeof value);
    return value;
}
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
        "Iq 0000000000000000000000000000000000001\r", "Iq 0.\00120\r",
        "send 0\r", "send 8\r", "send -1\r", "send\r", "send 0 extra\r", "send \r",
        "rpm nan\r", "rpm 9400.01\r", "pos 1000001\r", "position 10\r", "zero 1\r"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        unsigned rejected = app_command_rejected;
        usb(bad[i]);
        assert(app_command_rejected == rejected + 1u && foc.command == 0.35f);
    }
    usb("Iq 0."); ++g_usb_stats.sessions; usb("45\r");
    assert(foc.command == 0.35f); /* Old session prefix cannot start a command. */
    usb("Iq 0.50\r"); assert(foc.command == 0.5f);

    /* One compact 13-channel record is sent per 10 FOC samples (2 kHz). */
    for (unsigned n = 0; n < 20000u; ++n) {
        motor_sample_us = (0xffff00u + n * 50u) & 0xffffffu;
        app_sample();
        if ((n + 1u) % 10u == 0u) {
            assert((word() & 0xffffffu) == motor_sample_us);
            assert(((word() >> 24) & 7u) == foc.state);
            assert(index_word() == ((n + 1u) & 0xffffffu));
            assert(channel(2) == foc.id && channel(3) == foc.iq && channel(4) == foc.iq_ref);
            assert(channel(5) == foc.ud && channel(6) == foc.uq);
            assert(channel(7) == adc_sample.bus_voltage && channel(8) == mt6835_angle_deg);
            assert(channel(9) == foc.electrical_deg && channel(10) == control_speed_rpm());
            assert(channel(11) == control_speed_target() && channel(12) == control_position_deg());
            assert(channel(13) == control_position_target() && channel(14) == (float)control_mode());
        }
    }
    assert(frames == 2000u && foc.state == FOC_RUN);
    assert(s_uart_frames == 20u);

    /* Reference chain: the 10 A/s ramp converges, then holds the target. */
    assert(fabsf(foc.iq_ref - 0.5f) < 1e-4f);
    usb("Iq 0.20\r");
    float before_ramp = foc.iq_ref;
    motor_sample_us = (uint32_t)(motor_sample_us + 50u) & 0xffffffu;
    app_sample();
    assert(fabsf(foc.iq_ref - before_ramp + 5e-4f) < 1e-5f);
    for (unsigned n = 0; n < 6000u; ++n) {
        motor_sample_us = (uint32_t)(motor_sample_us + 50u) & 0xffffffu;
        app_sample();
    }
    assert(fabsf(foc.iq_ref - 0.2f) < 1e-4f && foc.command == 0.2f);
    /* Outer-loop output replaces the current reference, mode 3 reports it. */
    motor_sample_us = (motor_sample_us + 50u) & 0xffffffu;
    for (unsigned n = 0; n < 9u; ++n) {
        motor_sample_us = (motor_sample_us + 50u) & 0xffffffu;
        app_sample();
    }
    assert(channel(4) == foc.iq_ref && channel(10) == control_speed_rpm());
    assert(channel(11) == control_speed_target() && channel(13) == control_position_target());

    usb("stop\r"); assert(foc.state == FOC_IDLE && foc.command == 0.0f && motor_mode == MOTOR_OFF);
    assert(foc.iq_ref == 0.0f && control_mode() == 0u);
    usb("Iq 0.00\r"); assert(foc.state == FOC_IDLE);
    usb("Iq 0.20\r"); assert(foc.state == FOC_PRECHARGE && foc.command == 0.2f);
    usb("stop\r");
    usb("rpm 500\r"); assert(foc.state == FOC_PRECHARGE && control_mode() == 1u);
    assert(control_speed_target() == 500.0f && control_scheduled());
    usb("stop\r");
    usb("pos 720.00\r"); assert(foc.state == FOC_PRECHARGE && control_mode() == 2u);
    assert(control_position_target() == 720.0f);
    unsigned before_motion = app_command_rejected;
    usb("motion 7000 50000 500000\r"); assert(app_command_rejected == before_motion);
    assert(!app_command("motion 7000 0 500000"));
    usb("stop\r");
    assert(control_mode() == 0u && control_position_target() == 0.0f);
    /* `zero` needs the idle, stationary encoder state. */
    foc.zero_ready = true; foc.rpm = 0.0f; foc.state = FOC_IDLE;
    assert(app_command("zero") && control_position_deg() == 0.0f);
    foc.rpm = 100.0f; assert(!app_command("zero")); foc.rpm = 0.0f;
    /* `hello` answers on UART only, so the binary USB stream stays framed. */
    unsigned before = app_command_rejected;
    assert(app_command("hello") && app_command_rejected == before);
    /* Coast after stop: USB speed/position must keep following the encoder. */
    usb("rpm 250\r");
    for (unsigned n = 0; n < 2000u; ++n) {
        mt6835_angle_deg = fmodf(mt6835_angle_deg + 0.075f, 360.0f);
        motor_sample_us = (motor_sample_us + 50u) & 0xffffffu;
        app_sample();
    }
    assert(channel(10) > 200.0f);
    usb("stop\r");
    float coast_position = control_position_deg();
    for (unsigned n = 0; n < 6000u; ++n) {
        if (n < 2000u) mt6835_angle_deg = fmodf(mt6835_angle_deg + 0.03f, 360.0f);
        motor_sample_us = (motor_sample_us + 50u) & 0xffffffu;
        app_sample();
        assert(foc.state == FOC_IDLE && motor_mode == MOTOR_OFF && foc.iq_ref == 0.0f);
    }
    assert(fabsf(channel(10)) < 1.0f);
    assert(control_position_deg() - coast_position > 50.0f);
    puts("PASS: idle coast speed/position, 15-float 2 kHz USB layout, status word, rpm/pos/zero parsing,"
         " 20k frames, 10 A/s ramp, motion parsing, outer-loop reference selection");
    return 0;
}
