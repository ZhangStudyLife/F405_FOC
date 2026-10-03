#include "app.h"
#include "bsp_adc.h"
#include "bsp_motor.h"
#include "bsp_uart.h"
#include "control.h"
#include "music.h"
#include "mt6835_port_stm32.h"
#include "stm32f4xx_hal.h"
#include <math.h>
#include <string.h>

static unsigned telemetry_divider;
static volatile uint32_t last_sample;
static volatile uint32_t command_count, command_result;
static uint32_t last_command_ms;
enum { UNKNOWN, STOP, CLEAR, CAL, ZERO, TORQUE, SPEED, POSITION, SET_PARAM, SAVE, MUSIC_PLAY, MUSIC_STOP, MUSIC_PLAY_Q };

bool app_init(void)
{
    bsp_uart_init();
    if (!mt6835_init()) { app_fault(FOC_SENSOR); return false; }
    foc_calibration_t calibration;
    bool loaded = bsp_motor_load(&calibration, &motor_params);
    foc_init(loaded ? &calibration : NULL);
    bsp_adc_start();
    last_sample = HAL_GetTick();
    return true;
}

void app_fault(uint32_t fault)
{
    uint32_t key = bsp_motor_lock();
    bsp_motor_off();
    foc_trip(fault);
    bsp_motor_unlock(key);
}

/* 20 numeric channels and the JustFloat tail: 84 bytes, 500 Hz. */
static void __attribute__((noinline)) telemetry(void)
{
    float frame[] = {
        foc.id, foc.iq, foc.iq_ref + foc.iq_audio, adc_sample.bus_voltage,
        control.speed, control.speed_target, control.position, control.position_target,
        mt6835_raw_deg, adc_sample.b_voltage, adc_sample.c_voltage,
        foc.b_offset, foc.c_offset, foc.b_std_mv, foc.c_std_mv, 0.0f,
        (float)command_count, (float)command_result, (float)g_uart_stats.tx_rejected,
        0.0f, INFINITY
    };
    if (foc.state == FOC_FAULT) {
        memcpy(frame, foc_fault, 3u * sizeof(float));
        frame[19] = foc_fault[4];
    }
    uint32_t status = foc.state | (foc.fault << 3) | (control.mode << 7) |
        ((uint32_t)foc.zero_ready << 9) | ((uint32_t)foc.calibrated << 10) |
        ((uint32_t)(isfinite(mt6835_raw_deg) && isfinite(foc.angle_step)) << 11) |
        ((uint32_t)(isfinite(frame[0]) && isfinite(frame[1])) << 12) |
        ((uint32_t)(isfinite(frame[9]) && isfinite(frame[10]) && isfinite(frame[3])) << 13) |
        ((uint32_t)foc.zero_fault << 14) | ((uint32_t)foc.zero_complete << 22) |
        ((uint32_t)(motor_mode != MOTOR_OFF) << 23);
    if (foc.state == FOC_FAULT) status |= ((uint32_t)foc_fault[3] << 18) | ((uint32_t)foc_fault[5] << 20);
    frame[15] = (float)status;
    (void)bsp_uart_write(frame, sizeof frame);
}

void app_sample(void)
{
    last_sample = HAL_GetTick();
    unsigned previous_state = foc.state;
    float duty[3] = {motor_duty[0], motor_duty[1], motor_duty[2]};
    if (motor_mode == MOTOR_PWM && !foc_window(duty)) app_fault(FOC_WINDOW);
    foc_step(mt6835_angle_deg, adc_sample.bus_voltage, adc_sample.b_voltage,
             adc_sample.c_voltage, mt6835_sample_delay);
    uint32_t key = bsp_motor_lock();
    if (foc.fault) foc_trip(foc.fault);
    if (previous_state == FOC_OFFSET && foc.state == FOC_PRECHARGE) bsp_motor_arm();
    unsigned mode = foc.state == FOC_PRECHARGE ? MOTOR_PRECHARGE :
        (foc.state == FOC_RUN || foc.state == FOC_CALIBRATE) ? MOTOR_PWM : MOTOR_OFF;
    if (mode == MOTOR_OFF) bsp_motor_off();
    else if (!foc_window(foc.duty) && mode == MOTOR_PWM) app_fault(FOC_WINDOW);
    else if (!bsp_motor_write(foc.duty, mode)) app_fault(FOC_TIMING);
    bsp_motor_unlock(key);
    foc_outer_step();
    if (++telemetry_divider == 40u) {
        telemetry_divider = 0;
        telemetry();
    }
    bsp_uart_tick();
    bsp_motor_sample_end();
}

/* Decimal only, no exponent or non-finite values. Extra precision is needed for PI gains. */
static bool parse_decimal(const char *text, float *value)
{
    bool negative = *text == '-';
    if (*text == '-' || *text == '+') ++text;
    if (*text < '0' || *text > '9') return false;
    double number = 0, scale = 1;
    while (*text >= '0' && *text <= '9') number = number * 10 + (*text++ - '0');
    if (*text == '.') {
        ++text;
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            scale *= 0.1;
            number += (*text++ - '0') * scale;
        }
    }
    *value = (float)(negative ? -number : number);
    return !*text && isfinite(*value);
}

bool app_command(const char *line)
{
    last_command_ms = HAL_GetTick();
    GPIOD->BSRR = GPIO_PIN_2 << 16;
    unsigned command = UNKNOWN;
    float value = 0;
    float *parameter = NULL;
    bool valid = true;
    if (!strcmp(line, "stop")) command = STOP;
    else if (!strcmp(line, "clear")) command = CLEAR;
    else if (!strcmp(line, "cal")) command = CAL;
    else if (!strcmp(line, "zero")) command = ZERO;
    else if (!strcmp(line, "save")) command = SAVE;
    else if (!strcmp(line, "music play")) command = MUSIC_PLAY;
    else if (!strcmp(line, "music play q")) command = MUSIC_PLAY_Q;
    else if (!strcmp(line, "music stop")) command = MUSIC_STOP;
    else if (!strncmp(line, "iq ", 3)) { command = TORQUE; valid = parse_decimal(line + 3, &value); }
    else if (!strncmp(line, "rpm ", 4)) { command = SPEED; valid = parse_decimal(line + 4, &value); }
    else if (!strncmp(line, "pos ", 4)) { command = POSITION; valid = parse_decimal(line + 4, &value); }
    else if (!strncmp(line, "set ", 4)) {
        command = SET_PARAM;
#define PARAM(name) if (!strncmp(line + 4, #name " ", sizeof(#name))) { parameter = &motor_params.name; valid = parse_decimal(line + 4 + sizeof(#name), &value); }
        PARAM(current_kp)
        else PARAM(current_ki)
        else PARAM(speed_kp)
        else PARAM(speed_ki)
        else PARAM(position_kp)
        else PARAM(current_ramp)
        else PARAM(position_speed)
        else valid = false;
#undef PARAM
    } else valid = false;
    unsigned result = valid ? 3u : 2u;
    if (valid && command >= TORQUE && command <= POSITION &&
        fabsf(value) > (command == TORQUE ? FOC_CURRENT_MAX : command == SPEED ? FOC_SPEED_MAX : 1e6f)) {
        valid = false;
        result = 4;
    }
    uint32_t key = bsp_motor_lock();
    if (valid) switch (command) {
    case STOP: bsp_motor_off(); foc_stop(); break;
    case MUSIC_PLAY: case MUSIC_PLAY_Q:
        valid = MUSIC_ENABLE && foc.calibrated && foc.zero_ready &&
            (foc.state == FOC_IDLE || foc.state == FOC_RUN);
        if (command == MUSIC_PLAY_Q) valid = valid && control.mode == CONTROL_TORQUE &&
            foc.command == 0.0f && foc.iq_ref == 0.0f && fabsf(foc.rpm) < 5.0f;
        if (valid) {
            music_play(command == MUSIC_PLAY_Q);
            if (foc.state == FOC_IDLE) valid = foc_current(0.0f);
        }
        break;
    case MUSIC_STOP: music_stop(); foc.id_ref = foc.iq_audio = 0.0f; break;
    case CLEAR:
        valid = foc.state == FOC_FAULT && HAL_GetTick() - last_sample < 2u &&
            isfinite(mt6835_angle_deg) && isfinite(adc_sample.b_voltage) && isfinite(adc_sample.c_voltage) &&
            isfinite(adc_sample.bus_voltage) && adc_sample.bus_voltage >= FOC_BUS_MIN &&
            adc_sample.bus_voltage <= FOC_BUS_MAX && fabsf(foc.rpm) < FOC_SPEED_MAX &&
            (!foc.zero_ready || (fabsf(adc_sample.b_voltage - foc.b_offset) < 0.04f &&
             fabsf(adc_sample.c_voltage - foc.c_offset) < 0.04f &&
             fabsf(adc_sample.b_voltage + adc_sample.c_voltage - foc.b_offset - foc.c_offset) < 0.04f));
        if (valid) {
            motor_timing_fault = mt6835_timing_fault = 0u;
            mt6835_first_error = 0u; /* Capture the next fault after an explicit clear. */
            foc.fault = FOC_OK;
            if (foc.zero_ready) foc.state = FOC_IDLE;
            else {
                foc_calibration_t cal = foc.calibration;
                bool calibrated = foc.calibrated;
                foc_init(calibrated ? &cal : NULL);
                foc_stop();
            }
        }
        break;
    case CAL: valid = foc_calibrate(); break;
    case ZERO: valid = control_zero(); break;
    case TORQUE: case SPEED: case POSITION:
        valid = control_command(command - TORQUE, value); break;
    case SET_PARAM:
        valid = foc.state == FOC_IDLE && motor_mode == MOTOR_OFF;
        if (valid) {
            float old = *parameter;
            *parameter = value;
            valid = motor_params_valid(&motor_params);
            if (!valid) { *parameter = old; result = 4; }
        }
        break;
    case SAVE: valid = foc.state == FOC_IDLE && motor_mode == MOTOR_OFF && foc.calibrated; break;
    default: break;
    }
    if (valid && foc.state == FOC_PRECHARGE) bsp_motor_arm();
    bsp_motor_unlock(key);
    if (valid && command == SAVE) {
        key = bsp_motor_lock();
        bsp_adc_stop(); mt6835_stop();
        bsp_motor_unlock(key);
        valid = bsp_motor_save(&foc.calibration);
        if (!valid) { app_fault(FOC_FLASH); result = 5; }
        bsp_adc_start();
        last_sample = HAL_GetTick();
    }
    key = bsp_motor_lock();
    ++command_count;
    command_result = valid ? 1u : result;
    bsp_motor_unlock(key);
    return valid;
}

void app_poll(void)
{
    static char line[64];
    static unsigned length;
    static bool overflow;
    static uint32_t rx_errors;
    uint8_t ch;
    while (bsp_uart_read(&ch, 1)) {
        uint32_t errors = g_uart_stats.rx_errors + g_uart_stats.rx_lost;
        if (errors != rx_errors) { overflow = true; rx_errors = errors; }
        if (ch == '\r' || ch == '\n') {
            line[length] = 0;
            if (overflow) (void)app_command("");
            else if (length) (void)app_command(line);
            length = 0; overflow = false;
        } else if (ch >= 32 && ch < 127 && length < sizeof line - 1) line[length++] = (char)ch;
        else overflow = true;
    }
    if (HAL_GetTick() - last_command_ms >= 100u) GPIOD->BSRR = GPIO_PIN_2;
    if (foc.state == FOC_SAVE) {
        uint32_t key = bsp_motor_lock();
        bsp_motor_off(); bsp_adc_stop(); mt6835_stop();
        bsp_motor_unlock(key);
        bool ok = bsp_motor_save(&foc.calibration);
        key = bsp_motor_lock();
        if (ok) { foc.calibrated = true; if (!foc.fault) foc.state = FOC_IDLE; }
        else app_fault(FOC_FLASH);
        bsp_motor_unlock(key);
        bsp_adc_start();
        last_sample = HAL_GetTick();
    }
    /* A stalled chain must also leave OFFSET/IDLE; restart sampling with gates off. */
    if (HAL_GetTick() - last_sample >= 2u) {
        uint32_t key = bsp_motor_lock();
        if (foc.state != FOC_FAULT) app_fault(FOC_TIMING);
        bsp_adc_stop(); mt6835_stop();
        bsp_motor_unlock(key);
        bsp_adc_start();
        last_sample = HAL_GetTick();
    }
}
