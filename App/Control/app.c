#include "app.h"
#include "bsp_adc.h"
#include "bsp_can.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "mt6835_port_stm32.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static volatile uint32_t last_frame;
static volatile uint8_t divider;
static uint32_t sequence;
#ifdef FOC_CAPTURE
/* Debug build only; frozen before foreground export. No DMA reads this array. */
typedef struct { uint32_t sequence; float b, c, bus, theta, alpha, beta; uint32_t flags; } capture_t;
_Static_assert(sizeof(capture_t) == 32u, "capture wire layout");
static capture_t capture[2048];
static volatile unsigned capture_count;
static volatile bool capturing, dumping;
static unsigned dump_index;
#endif
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
#ifdef FOC_CAPTURE
    capturing = false; /* Freeze even if ADC/SPI stops producing callbacks. */
#endif
}

void app_sample(void)
{
    unsigned sampled_mode = motor_mode, previous_state = foc.state;
    float duty[3] = {motor_duty[0], motor_duty[1], motor_duty[2]};
    bool valid = sampled_mode != MOTOR_PWM || foc_window(duty);
    if (!valid) app_fault(FOC_WINDOW); /* Validate measured period BEFORE PI integration. */
    foc_step(mt6835_angle_deg, adc_sample.bus_voltage, adc_sample.b_voltage,
             adc_sample.c_voltage, mt6835_sample_delay);
    uint32_t key = bsp_motor_lock();
    if (foc.fault) foc_trip(foc.fault); /* Preserve a preempting priority-0 fault. */
    if (previous_state == FOC_OFFSET && foc.state == FOC_PRECHARGE) bsp_motor_arm();
    unsigned mode = foc.state == FOC_PRECHARGE ? MOTOR_PRECHARGE :
        (foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) ? MOTOR_PWM : MOTOR_OFF;
    if (mode == MOTOR_OFF) bsp_motor_off();
    else if (!foc_window(foc.duty) && mode == MOTOR_PWM) app_fault(FOC_WINDOW);
    else if (!bsp_motor_write(foc.duty, mode)) app_fault(FOC_TIMING);
    bsp_motor_unlock(key);
    sequence = (sequence + 1u) & 0xffffffu;
#ifdef FOC_CAPTURE
    if (capturing) {
        float bus = adc_sample.bus_voltage;
        capture[capture_count++] = (capture_t){sequence, adc_sample.b_voltage, adc_sample.c_voltage,
            bus, foc.electrical_deg,
            sampled_mode == MOTOR_PWM ? bus * (2.0f * duty[0] - duty[1] - duty[2]) / 3.0f : 0.0f,
            sampled_mode == MOTOR_PWM ? bus * (duty[1] - duty[2]) * 0.5773502692f : 0.0f,
            foc.state | (foc.fault << 8) | (sampled_mode << 16) | ((uint32_t)valid << 24)};
        if (capture_count == 2048u || foc.fault) {
            capturing = false;
            key = bsp_motor_lock(); bsp_motor_off(); foc_stop(); bsp_motor_unlock(key);
        }
    }
#endif
    if (++divider == 10u) {
        divider = 0u;
        last_frame = bsp_uart_millis();
#ifdef FOC_CAPTURE
        if (!dumping)
#endif
        {
            /* New Ud/Uq are NEXT-period outputs, not this sample's applied voltage. */
            const float frame[] = {(float)sequence, foc.command, foc.iq_ref, foc.id, foc.iq,
                foc.ud, foc.uq, adc_sample.bus_voltage, adc_sample.b_voltage, adc_sample.c_voltage,
                foc.electrical_deg, foc.rpm, (float)foc.state, (float)foc.fault, INFINITY};
            (void)bsp_uart_write(frame, sizeof frame);
        }
    }
    bsp_motor_sample_end();
}

/* Foreground parser; only state changes use the short critical section. */
bool app_command(const char *line)
{
    enum { STOP, RUN, CAL, CLEAR } command;
    float amps = 0.0f;
#ifdef FOC_CAPTURE
    if (!strcmp(line, "capture")) {
        uint32_t key = bsp_motor_lock();
        bool ok = !capturing && !dumping && foc.state != FOC_FAULT && foc.zero_ready;
        if (ok) { capture_count = 0u; capturing = true; }
        bsp_motor_unlock(key); return ok;
    }
    if (!strcmp(line, "dump")) {
        uint32_t key = bsp_motor_lock();
        bool ok = !capturing && !dumping && capture_count && (foc.state == FOC_IDLE || foc.state == FOC_FAULT);
        if (ok) { dump_index = 0u; dumping = true; }
        bsp_motor_unlock(key); return ok;
    }
    if (dumping && strcmp(line, "stop")) return false;
#endif
    if (!strcmp(line, "stop")) command = STOP;
    else if (!strcmp(line, "cal")) command = CAL;
    else if (!strcmp(line, "clear")) command = CLEAR;
    else if (!strncmp(line, "Iq ", 3u)) {
        /* Decimal only: optional sign, digits, optionally 1 or 2 decimals. */
        const char *p = line + 3;
        if (*p == '-' || *p == '+') ++p;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') {
            unsigned decimals = 0u;
            while (*++p >= '0' && *p <= '9') ++decimals;
            if (!decimals || decimals > 2u) return false;
        }
        if (*p) return false;
        char *end;
        amps = strtof(line + 3, &end);
        if (end == line + 3 || *end || !isfinite(amps) || fabsf(amps) > 0.8f) return false;
        command = RUN;
    } else return false;
    uint32_t key = bsp_motor_lock();
    bool ok = true;
    if (command == STOP) { bsp_motor_off(); foc_stop(); }
    else if (command == RUN) ok = foc_current(amps);
    else if (command == CAL) ok = foc_calibrate();
    else {
        ok = foc.state == FOC_FAULT && bsp_uart_millis() - last_frame < 2u && isfinite(mt6835_angle_deg) &&
             isfinite(adc_sample.b_voltage) && isfinite(adc_sample.c_voltage) &&
             isfinite(adc_sample.bus_voltage) && adc_sample.bus_voltage >= 6.0f && adc_sample.bus_voltage <= 14.0f &&
             fabsf(foc.rpm) < 3000.0f &&
             (!foc.zero_ready || (fabsf(adc_sample.b_voltage - foc.b_offset) < 0.04f &&
              fabsf(adc_sample.c_voltage - foc.c_offset) < 0.04f &&
              fabsf(adc_sample.b_voltage + adc_sample.c_voltage - foc.b_offset - foc.c_offset) < 0.04f));
        if (ok) {
            foc.fault = FOC_OK;
            if (foc.zero_ready) foc.state = FOC_IDLE;
            else {
                foc_calibration_t calibration = foc.calibration;
                bool calibrated = foc.calibrated;
                foc_init(calibrated ? &calibration : NULL);
                foc_stop(); /* Clear never restarts alignment or motor operation. */
            }
        }
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
#ifdef FOC_CAPTURE
    if (dumping) {
        /* At most one chunk/ms, below UART capacity. Normal telemetry is paused.
           Header: FOC1 + little-endian record count, then exact 32-byte records. */
        static uint32_t sent_at;
        uint32_t now = bsp_uart_millis();
        if (now != sent_at) {
            sent_at = now;
            if (dump_index == 0u) {
                uint32_t header[2] = {0x31434f46u, capture_count};
                if (bsp_uart_write(header, sizeof header)) dump_index = 1u;
            } else if (dump_index <= capture_count) {
                unsigned count = capture_count - dump_index + 1u;
                if (count > 4u) count = 4u;
                if (bsp_uart_write(&capture[dump_index - 1u], count * sizeof(capture_t))) dump_index += count;
            } else dumping = false;
        }
    }
#endif
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
