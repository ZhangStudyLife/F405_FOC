#include "app.h"
#include "bsp_adc.h"
#include "bsp_can.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "mt6835_port_stm32.h"
#include "justfloat.h"
#include <stdlib.h>
#include <string.h>

static volatile uint32_t last_frame;
static volatile uint8_t divider;
volatile uint32_t app_command_rejected;

bool app_init(void)
{
    if (!bsp_uart_init() || !bsp_can_init() || !mt6835_init()) return false;
    foc_calibration_t calibration;
    foc_init(bsp_motor_load(&calibration) ? &calibration : NULL);
    bsp_adc_start();
    return true;
}

void app_fault(uint32_t fault)
{
    bsp_motor_off();
    foc_trip(fault);
}

void app_sample(void)
{
    /* Latched at the preceding valley, before this ADC aperture. */
    float a = motor_duty[0], b = motor_duty[1], c = motor_duty[2], uq = foc.uq;
    foc_step(mt6835_angle_deg);
    uint32_t key = bsp_motor_lock();
    /* Preserve a fault that preempted the calibration/state calculation. */
    if (foc.fault) foc_trip(foc.fault);
    unsigned mode = foc.state == FOC_PRECHARGE ? MOTOR_PRECHARGE :
        (foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) ? MOTOR_PWM : MOTOR_OFF;
    if (mode == MOTOR_OFF) bsp_motor_off();
    else if (!foc_window(foc.duty) && mode == MOTOR_PWM) app_fault(FOC_WINDOW);
    else if (!bsp_motor_write(foc.duty, mode)) app_fault(FOC_TIMING);
    bsp_motor_unlock(key);
    if (++divider == 20u) {
        divider = 0u;
        last_frame = bsp_uart_millis();
        (void)justfloat_send(a, b, c, mt6835_angle_deg,
            adc_sample.b_voltage, adc_sample.c_voltage, foc.rpm, uq,
            (float)foc.state, (float)foc.fault);
    }
    bsp_motor_sample_end();
}

/* Foreground parser; only state changes use the short critical section. */
bool app_command(const char *line)
{
    enum { STOP, RUN, CAL, CLEAR } command;
    float volts = 0.0f;
    if (!strcmp(line, "stop")) command = STOP;
    else if (!strcmp(line, "cal")) command = CAL;
    else if (!strcmp(line, "clear")) command = CLEAR;
    else if (!strncmp(line, "run ", 4u)) {
        char *end;
        volts = strtof(line + 4, &end);
        if (end == line + 4 || *end || !isfinite(volts) || fabsf(volts) > 0.6f) return false;
        command = volts == 0.0f ? STOP : RUN;
    } else return false;
    uint32_t key = bsp_motor_lock();
    bool ok = true;
    if (command == STOP) { bsp_motor_off(); foc_stop(); }
    else if (command == RUN) ok = foc_run(volts);
    else if (command == CAL) ok = foc_calibrate();
    else {
        ok = foc.state == FOC_FAULT && bsp_uart_millis() - last_frame < 2u && isfinite(mt6835_angle_deg) &&
             isfinite(adc_sample.b_voltage) && isfinite(adc_sample.c_voltage);
        if (ok) { foc.fault = FOC_OK; foc.state = FOC_IDLE; }
    }
    if (ok && foc.state == FOC_PRECHARGE) bsp_motor_arm();
    bsp_motor_unlock(key);
    return ok;
}

void app_poll(void)
{
    static char line[32];
    static unsigned length;
    static bool overflow;
    static uint32_t rx_errors;
    uint32_t errors = g_uart_stats.rx_errors + g_uart_stats.rx_lost;
    if (errors != rx_errors) {
        rx_errors = errors;
        overflow = true; /* Discard the damaged command through its newline. */
        uint32_t key = bsp_motor_lock();
        if (foc.state == FOC_PRECHARGE || foc.state == FOC_CALIBRATE || foc.state == FOC_RUN)
            app_fault(FOC_UART);
        bsp_motor_unlock(key);
    }
    uint8_t ch;
    while (bsp_uart_read(&ch, 1u)) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[length] = '\0';
            if (overflow || (length && !app_command(line))) ++app_command_rejected;
            length = 0u; overflow = false;
        } else if (length < sizeof line - 1u && ch >= 32u && ch < 127u) line[length++] = (char)ch;
        else overflow = true;
    }
    if (foc.state == FOC_SAVE && divider == 0u) {
        uint32_t key = bsp_motor_lock();
        bsp_adc_stop();
        mt6835_stop();
        bsp_motor_unlock(key);
        bool ok = bsp_motor_save(&foc.calibration);
        if (ok) { foc.calibrated = true; foc.state = FOC_IDLE; }
        else app_fault(FOC_FLASH);
        divider = 0u;
        bsp_adc_start();
    }
    /* Recover acquisition after a stalled DMA/ADC, never motor operation. */
    if (foc.state == FOC_FAULT && bsp_uart_millis() - last_frame >= 2u) {
        uint32_t key = bsp_motor_lock();
        bsp_adc_stop(); mt6835_stop();
        bsp_motor_unlock(key);
        divider = 0u;
        bsp_adc_start();
        last_frame = bsp_uart_millis();
    }
}
