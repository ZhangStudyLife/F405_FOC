#include "app.h"
#include "bsp_adc.h"
#include "bsp_can.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "bsp_usb.h"
#include "control.h"
#include "justfloat.h"
#include "mt6835_port_stm32.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOC_FRAME_CHANNELS 12u /* 2 header words plus 10 payload channels. */
/* FOC_EDGE_LIMIT / 4200 from foc.c: sample-window ceiling on the vector span. */
#define FOC_EDGE_FRACTION 0.1211428571f

/* High-speed USB logging group, selected by `send X`; telemetry only. */
static volatile uint8_t telemetry_group;
/* One USB frame: two header words then the group payload, all float32. */
static float s_usb_frame[FOC_FRAME_CHANNELS + 1u];

static volatile uint32_t last_frame;
static volatile uint8_t divider;
static uint32_t sequence;
#ifdef FOC_CAPTURE
/* Debug build only; frozen before foreground export. No DMA reads this array. */
typedef struct {
    uint32_t sequence;
    uint16_t raw[3], counter, ccr[3], flags;
    float mechanical, theta, id, iq, reference, ud, uq;
} capture_t;
_Static_assert(sizeof(capture_t) == 48u, "capture wire layout");
static capture_t capture[2048];
static volatile unsigned capture_count;
static volatile bool capturing, dumping, quiet, capture_low;
static unsigned dump_index;
#endif
volatile uint32_t app_command_rejected;

bool app_init(void)
{
    if (!bsp_uart_init() || !bsp_can_init()) return false;
    if (!mt6835_init()) { app_fault(FOC_SENSOR); return false; }
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

/* Same bit layout as the historical telemetry status word; it round-trips
   exactly through the float32 channel, so no separate integer frame exists. */
static uint32_t status_word(void)
{
    return foc.state | (foc.fault << 3) | ((uint32_t)(motor_mode == MOTOR_PWM) << 7);
}

/* 20 kHz USB logging: one group at a time, 12 float32 plus the JustFloat
   terminator. Group 0 carries raw sensor truth, group 1 current-loop internals,
   group 2 the applied voltage, group 3 the reference and mechanical response.
   Channels that a host can reconstruct offline are deliberately absent. */
static void telemetry_usb(const float sampled_duty[3])
{
    union { float f; uint32_t u; } time, index;
    time.u = (motor_sample_us & 0xffffffu) | (status_word() << 24);
    index.u = ((sequence & 0xffffffu) | ((uint32_t)telemetry_group << 24));
    float *payload = s_usb_frame;
    payload[0] = time.f;
    payload[1] = index.f;
    switch (telemetry_group) {
    default: /* Group 0: current, bus, encoder, and PWM of the sampled period. */
        payload[2] = (float)adc_raw_b;
        payload[3] = (float)adc_raw_c;
        payload[4] = (float)adc_raw_bus;
        payload[5] = mt6835_raw_deg;
        payload[6] = mt6835_angle_deg;
        payload[7] = (float)(uint32_t)(sampled_duty[0] * 4200.0f + 0.5f);
        payload[8] = (float)(uint32_t)(sampled_duty[1] * 4200.0f + 0.5f);
        payload[9] = (float)(uint32_t)(sampled_duty[2] * 4200.0f + 0.5f);
        payload[10] = foc.b_offset;
        payload[11] = foc.c_offset;
        break;
    case 1: { /* Phase currents, electrical angle, PI state, pre-limit command. */
        float integral_d, integral_q;
        foc_integrators(&integral_d, &integral_q);
        payload[2] = foc.zero_ready ? (adc_sample.b_voltage - foc.b_offset) * 50.0f : NAN;
        payload[3] = foc.zero_ready ? (adc_sample.c_voltage - foc.c_offset) * 50.0f : NAN;
        payload[4] = foc.electrical_deg;
        payload[5] = foc.iq_ref;
        payload[6] = integral_d;
        payload[7] = integral_q;
        payload[8] = foc.ud;
        payload[9] = foc.uq;
        payload[10] = foc.b_offset;
        payload[11] = foc.c_offset;
        break;
    }
    case 2: /* Applied voltage: the CCRs live for this period, and the limits. */
        payload[2] = foc.ud;
        payload[3] = foc.uq;
        payload[4] = (float)(uint32_t)(foc.duty[0] * 4200.0f + 0.5f);
        payload[5] = (float)(uint32_t)(foc.duty[1] * 4200.0f + 0.5f);
        payload[6] = (float)(uint32_t)(foc.duty[2] * 4200.0f + 0.5f);
        payload[7] = motor_duty[0];
        payload[8] = motor_duty[1];
        payload[9] = motor_duty[2];
        payload[10] = FOC_EDGE_FRACTION * adc_sample.bus_voltage;
        payload[11] = 0.5773502692f * adc_sample.bus_voltage;
        break;
    case 3: /* Reference chain and mechanical response. */
        payload[2] = foc.iq_ref;
        payload[3] = foc.command;
        payload[4] = control_position_deg();
        payload[5] = control_position_target();
        payload[6] = control_speed_rpm();
        payload[7] = foc.rpm;
        payload[8] = control_speed_target();
        payload[9] = foc.iq;
        payload[10] = (float)control_mode();
        payload[11] = adc_sample.bus_voltage;
        break;
    }
    s_usb_frame[FOC_FRAME_CHANNELS] = INFINITY;
    (void)bsp_usb_write(&s_usb_frame, sizeof s_usb_frame);
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
#ifdef FOC_CAPTURE
    if (capturing && capture_low && !foc.fault) mode = MOTOR_PRECHARGE;
#endif
    if (mode == MOTOR_OFF) bsp_motor_off();
    else if (!foc_window(foc.duty) && mode == MOTOR_PWM) app_fault(FOC_WINDOW);
    else if (!bsp_motor_write(foc.duty, mode)) app_fault(FOC_TIMING);
    bsp_motor_unlock(key);
    foc_outer_step();
    sequence = (sequence + 1u) & 0xffffffu;
    if (bsp_usb_ready()) telemetry_usb(duty); /* PWM sampled before this cycle's write. */
#ifdef FOC_CAPTURE
    if (capturing) {
        capture[capture_count++] = (capture_t){sequence,
            {adc_debug[0], adc_debug[1], adc_debug[2]}, adc_debug[3],
            {(uint16_t)(duty[0]*4200.0f+0.5f), (uint16_t)(duty[1]*4200.0f+0.5f), (uint16_t)(duty[2]*4200.0f+0.5f)},
            (uint16_t)(foc.state | (foc.fault << 3) | (sampled_mode << 7) | ((uint32_t)valid << 9)),
            mt6835_angle_deg, foc.electrical_deg, foc.id, foc.iq, foc.iq_ref, foc.ud, foc.uq};
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
        if (!dumping && !quiet)
#endif
        {
            /* 2 kHz, 64 bytes: 64% of 2 Mbps, including 8N1 overhead. */
            const float frame[] = {foc.id, foc.iq, adc_sample.b_voltage, adc_sample.c_voltage,
                (float)sequence, foc.rpm, adc_sample.bus_voltage, foc.iq_ref, foc.electrical_deg,
                foc.ud, foc.uq, duty[0], duty[1], duty[2],
                (float)status_word(), INFINITY};
            (void)bsp_uart_write(frame, sizeof frame);
            bsp_uart_tick(); /* Fixed sample phase, after ADC and PWM submission. */
        }
    }
    bsp_motor_sample_end();
}

/* Decimal only: optional sign, digits, optionally 1 or 2 decimals. Shared by
   every numeric command so each one gets the same validation. */
static bool parse_decimal(const char *p, float *value, float limit)
{
    const char *start = p;
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
    float parsed = strtof(start, &end);
    if (end != p || !isfinite(parsed) || fabsf(parsed) > limit) return false;
    *value = parsed;
    return true;
}

/* Foreground parser; only state changes use the short critical section. */
bool app_command(const char *line)
{
    enum { STOP, CAL, CLEAR, ZERO, TORQUE, SPEED, POS, MOTION, HELLO } command;
    float value = 0.0f, acceleration = 0.0f, jerk = 0.0f;
#ifdef FOC_CAPTURE
    if (!strcmp(line, "quiet 0") || !strcmp(line, "quiet 1")) {
        quiet = line[6] == '1'; return true;
    }
    bool low = !strcmp(line, "capture low"), zero = !strcmp(line, "capture zero");
    if (!strcmp(line, "capture") || low || zero) {
        uint32_t key = bsp_motor_lock();
        bool ok = !capturing && !dumping && foc.state != FOC_FAULT && foc.zero_ready;
        if (low || zero) ok = ok && foc.state == FOC_IDLE && fabsf(foc.rpm) < 5.0f;
        if (ok && (low || zero)) {
            ok = foc_current(0.01f);
            if (ok) { foc.command = 0.0f; bsp_motor_arm(); }
        }
        if (ok) { capture_low = low; capture_count = 0u; capturing = true; }
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
    else if (!strcmp(line, "zero")) command = ZERO;
    else if (!strcmp(line, "hello")) command = HELLO;
    else if (!strncmp(line, "Iq ", 3u)) { command = TORQUE; if (!parse_decimal(line + 3, &value, FOC_CURRENT_MAX)) return false; }
    else if (!strncmp(line, "rpm ", 4u)) { command = SPEED; if (!parse_decimal(line + 4, &value, FOC_SPEED_MAX)) return false; }
    else if (!strncmp(line, "pos ", 4u)) { command = POS; if (!parse_decimal(line + 4, &value, 1e6f)) return false; }
    else if (!strncmp(line, "motion ", 7u)) {
        char fields[32];
        strncpy(fields, line + 7, sizeof fields);
        fields[sizeof fields - 1u] = '\0';
        char *a = strchr(fields, ' ');
        if (!a) return false;
        *a++ = '\0';
        char *j = strchr(a, ' ');
        if (!j) return false;
        *j++ = '\0';
        if (!parse_decimal(fields, &value, FOC_SPEED_MAX) ||
            !parse_decimal(a, &acceleration, 100000.0f) ||
            !parse_decimal(j, &jerk, 1000000.0f)) return false;
        command = MOTION;
    }
    else if (!strncmp(line, "send ", 5u) && line[5] >= '0' && line[5] <= '3' && !line[6]) {
        telemetry_group = (uint8_t)(line[5] - '0');
        return true;
    } else return false;
    uint32_t key = bsp_motor_lock();
    bool ok = true;
    switch (command) {
    case STOP:
#ifdef FOC_CAPTURE
        capturing = capture_low = quiet = false;
#endif
        bsp_motor_off(); foc_stop();
        break;
    case TORQUE: ok = control_torque(value); break;
    case SPEED: ok = control_speed(value); break;
    case POS: ok = control_position(value); break;
    case MOTION: ok = control_motion(value, acceleration, jerk); break;
    case ZERO: ok = control_zero(); break;
    case CAL: ok = foc_calibrate(); break;
    case HELLO: break; /* No state change; the caller prints the banner. */
    case CLEAR:
        ok = foc.state == FOC_FAULT && bsp_uart_millis() - last_frame < 2u && isfinite(mt6835_angle_deg) &&
             isfinite(adc_sample.b_voltage) && isfinite(adc_sample.c_voltage) &&
             isfinite(adc_sample.bus_voltage) && adc_sample.bus_voltage >= FOC_BUS_MIN && adc_sample.bus_voltage <= FOC_BUS_MAX &&
             fabsf(foc.rpm) < FOC_SPEED_MAX &&
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
        break;
    }
    if (ok && foc.state == FOC_PRECHARGE) bsp_motor_arm();
    bsp_motor_unlock(key);
    if (command == HELLO) {
        /* Text goes to UART only: the USB link is a binary frame stream. */
        char banner[24];
        unsigned length = (unsigned)snprintf(banner, sizeof banner, "#FOC 1.1 %lu\r\n",
                                             (unsigned long)status_word());
        (void)bsp_uart_write(banner, length);
        bsp_uart_tick();
    }
    return ok;
}

typedef struct {
    char line[32];
    unsigned length;
    bool overflow;
} command_line_t;

static void command_byte(command_line_t *rx, uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        rx->line[rx->length] = '\0';
        bool rejected = rx->overflow || (rx->length && !app_command(rx->line));
        if (rejected) ++app_command_rejected;
        rx->length = 0u;
        rx->overflow = false;
    } else if (rx->length < sizeof rx->line - 1u && ch >= 32u && ch < 127u) {
        rx->line[rx->length++] = (char)ch;
    } else rx->overflow = true;
}

void app_poll(void)
{
    bsp_usb_poll();
    static command_line_t uart_rx, usb_rx;
    static uint32_t usb_session;
    static uint32_t rx_errors;
    uint32_t errors = g_uart_stats.rx_errors + g_uart_stats.rx_lost;
    if (errors != rx_errors) {
        rx_errors = errors;
        uart_rx.overflow = true; /* Discard the damaged command through its terminator. */
        uint32_t key = bsp_motor_lock();
        if (foc.state == FOC_PRECHARGE || foc.state == FOC_CALIBRATE || foc.state == FOC_RUN)
            app_fault(FOC_UART);
        bsp_motor_unlock(key);
    }
    uint8_t ch;
    while (bsp_uart_read(&ch, 1u)) {
        command_byte(&uart_rx, ch);
    }
    if (usb_session != g_usb_stats.sessions) {
        usb_session = g_usb_stats.sessions;
        usb_rx.length = 0u;
        usb_rx.overflow = false; /* Never join a partial line across DTR sessions. */
    }
    uint8_t packet[64];
    size_t received = bsp_usb_read(packet, sizeof packet);
    for (size_t i = 0; i < received; ++i) command_byte(&usb_rx, packet[i]);
#ifdef FOC_CAPTURE
    if (dumping) {
        /* At most one chunk/ms, below UART capacity. Normal telemetry is paused.
           Header: FOC3 + little-endian record count, then exact 48-byte records. */
        static uint32_t sent_at;
        uint32_t now = bsp_uart_millis();
        if (now != sent_at) {
            sent_at = now;
            if (dump_index == 0u) {
                uint32_t header[2] = {0x33434f46u, capture_count};
                if (bsp_uart_write(header, sizeof header)) dump_index = 1u;
            } else if (dump_index <= capture_count) {
                unsigned count = capture_count - dump_index + 1u;
                if (count > 3u) count = 3u;
                if (bsp_uart_write(&capture[dump_index - 1u], count * sizeof(capture_t))) dump_index += count;
            } else dumping = false;
            bsp_uart_tick();
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
