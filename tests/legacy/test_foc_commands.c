/* Historical voltage-mode test; incompatible with the current current-mode API. */
#include "app.h"
#include "bsp_adc.h"
#include "bsp_motor.h"
#include "bsp_motor_record.h"
#include "bsp_uart.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

volatile bsp_uart_stats_t g_uart_stats;
volatile bsp_adc_sample_t adc_sample = {1.65f, 1.65f, 12.6f};
volatile float mt6835_angle_deg = 20.0f, motor_duty[3];
static unsigned stopped, frames;
static float latest[6];
uint32_t bsp_motor_lock(void) { return 0u; }
void bsp_motor_unlock(uint32_t key) { (void)key; }
void bsp_motor_arm(void) {}
void bsp_motor_off(void) { ++stopped; }
bool bsp_motor_load(foc_calibration_t *cal) { (void)cal; return false; }
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
size_t bsp_uart_read(void *data, size_t size) { (void)data; (void)size; return 0u; }
bool bsp_uart_write(const void *data, size_t size) { assert(size == sizeof latest); memcpy(latest, data, size); ++frames; return true; }

int main(void)
{
    foc_calibration_t cal = {.zero = 1.0f, .direction = -1};
    foc_init(&cal);
    const char *bad[] = {"", "RUN 0.3", "run", "run ", "run nan", "run inf", "run 1e99", "Set 4.81", "Set -4.81", "Set 7.27", "Set 0.123", "Set 1e0", "Set .3", "Set 1.", "Set nan", "run 0.3junk", "cal extra"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        assert(!app_command(bad[i])); assert(foc.state == FOC_IDLE);
    }
    assert(app_command("Set 0.15"));
    assert(foc.state == FOC_PRECHARGE);
    assert(!app_command("cal"));
    for (unsigned i = 0; i < 100; ++i) foc_step(20.0f, 12.6f);
    assert(!app_command("run -0.15"));
    assert(app_command("stop") && stopped == 1u && foc.state == FOC_IDLE);
    assert(app_command("Set -0.30"));
    assert(app_command("Set 0") && stopped == 2u);
    app_fault(FOC_SENSOR);
    mt6835_angle_deg = NAN;
    assert(!app_command("clear"));
    mt6835_angle_deg = 20.0f;
    assert(app_command("clear") && foc.state == FOC_IDLE);

    assert(app_command("run 0.3"));
    ++g_uart_stats.rx_errors;
    app_poll();
    assert(foc.state == FOC_FAULT && foc.fault == FOC_UART);
    assert(app_command("clear") && foc.state == FOC_IDLE);

    foc_stop();
    for (unsigned n = 0; n < 20000; ++n) app_sample();
    assert(frames == 2000 && latest[0] == 0.0f && latest[1] == 12.6f);
    assert(latest[2] == 1.65f && latest[3] == 1.65f);
    assert(latest[4] >= 0.0f && latest[4] < 360.0f && isinf(latest[5]));
    assert(app_command("Set 4.80"));
    assert(foc.command == 4.8f);
    assert(app_command("Set 0"));
    assert(app_command("Set -4.80"));
    assert(foc.command == -4.8f);
    assert(app_command("Set 0"));
    record_t record = {.version = 1u, .poles = 7u, .cal = cal, .magic = 0x464f4331u};
    record.checksum = checksum(&record);
    assert(record_valid(&record));
    for (unsigned bit = 0; bit < sizeof record * 8u; ++bit) {
        record_t damaged = record;
        ((unsigned char *)&damaged)[bit / 8u] ^= 1u << (bit % 8u);
        assert(!record_valid(&damaged));
    }
    record.cal.zero = NAN; record.checksum = checksum(&record); assert(!record_valid(&record));
    record.cal = cal; record.cal.direction = 0; record.checksum = checksum(&record); assert(!record_valid(&record));
    memset(&record, 0xff, sizeof record); assert(!record_valid(&record));
    puts("FOC commands, stop/clear, corrupt/torn calibration records: PASS");
}
