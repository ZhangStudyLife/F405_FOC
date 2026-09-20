#ifndef APP_HARDWARE_BSP_MOTOR_H
#define APP_HARDWARE_BSP_MOTOR_H

#include "foc.h"
enum { MOTOR_OFF, MOTOR_PRECHARGE, MOTOR_PWM };
extern volatile float motor_duty[3]; /* Active at the last PWM valley. */
extern volatile uint32_t motor_cycles, motor_period_min, motor_period_max, motor_work_max;
uint32_t bsp_motor_lock(void);
void bsp_motor_unlock(uint32_t key);
void bsp_motor_init(void);
void bsp_motor_arm(void); /* Foreground, IRQ locked, accepted start/cal only. */
void bsp_motor_off(void); /* Immediate, also safe from fault handlers. */
bool bsp_motor_write(const float duty[3], unsigned mode);
bool bsp_motor_update(void); /* TIM8 update IRQ: watchdog and atomic latch. */
bool bsp_motor_sample_begin(void);
void bsp_motor_sample_end(void);
bool bsp_motor_load(foc_calibration_t *calibration);
bool bsp_motor_save(const foc_calibration_t *calibration); /* Stopped foreground only. */

#endif
