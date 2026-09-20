#include "bsp_motor.h"
#include "bsp_motor_record.h"
#include "stm32f4xx_hal.h"
#include <string.h>
#include <stddef.h>

uint32_t bsp_motor_lock(void) { uint32_t key = __get_PRIMASK(); __disable_irq(); return key; }
void bsp_motor_unlock(uint32_t key) { __set_PRIMASK(key); }

#define GATE_CHANNELS (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | TIM_CCER_CC3E | TIM_CCER_CC3NE)
static volatile unsigned pending_mode;
volatile unsigned motor_mode;
static volatile bool ready, inhibited;
static uint32_t sample_start, last_sample;
volatile float motor_duty[3];
volatile uint32_t motor_cycles, motor_period_min = UINT32_MAX, motor_period_max, motor_work_max;

void bsp_motor_arm(void) { inhibited = false; }

void bsp_motor_off(void)
{
    inhibited = true;
    TIM8->CCER &= ~GATE_CHANNELS; /* CH4 remains running for acquisition. */
    GPIOA->BSRR = GPIO_PIN_7 << 16;
    GPIOB->BSRR = (GPIO_PIN_0 | GPIO_PIN_1) << 16;
    GPIOC->BSRR = (GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8) << 16;
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 14)) | (1u << 14);
    GPIOB->MODER = (GPIOB->MODER & ~15u) | 5u;
    GPIOC->MODER = (GPIOC->MODER & ~(63u << 12)) | (21u << 12);
    motor_mode = pending_mode = MOTOR_OFF;
    for (unsigned i = 0; i < 3; ++i) motor_duty[i] = 0.0f;
}

void bsp_motor_init(void)
{
    bsp_motor_off();
    ready = false;
    inhibited = false; /* Initial sampling starts with gates disconnected. */
    last_sample = 0u;
    motor_period_min = UINT32_MAX;
    motor_period_max = motor_work_max = motor_cycles = 0u;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    /* UG loads RCR=1 at CNT=0: overflow counts down, underflow latches CCRs.
       Force CH4 low before toggle mode so rising trigger is on the up-count. */
    TIM8->CR1 = TIM_CR1_CMS_0 | TIM_CR1_ARPE;
    TIM8->PSC = 0u; TIM8->ARR = 4200u; TIM8->RCR = 1u;
    TIM8->CCMR1 = TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE | (6u << 4) | (6u << 12);
    TIM8->CCMR2 = TIM_CCMR2_OC3PE | (6u << 4) | (4u << 12);
    TIM8->CCR1 = TIM8->CCR2 = TIM8->CCR3 = 2100u;
    TIM8->CCR4 = FOC_TRIGGER_TICKS;
    TIM8->BDTR = 84u; /* 84 / 168 MHz = 500 ns. No automatic restart. */
    TIM8->CNT = 0u; TIM8->EGR = TIM_EGR_UG;
    TIM8->CCMR2 = TIM_CCMR2_OC3PE | (6u << 4) | (3u << 12);
    TIM8->SR = 0u;
    TIM8->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM8_UP_TIM13_IRQn, 0u, 0u);
    HAL_NVIC_ClearPendingIRQ(TIM8_UP_TIM13_IRQn);
    HAL_NVIC_EnableIRQ(TIM8_UP_TIM13_IRQn);
}

bool bsp_motor_write(const float duty[3], unsigned mode)
{
    /* All three preloads must be written before the next valley, never across it. */
    if (inhibited || !(TIM8->CR1 & TIM_CR1_DIR) || TIM8->CNT < 600u) return false;
    TIM8->CCR1 = mode == MOTOR_PWM ? (uint32_t)(duty[0] * 4200.0f + 0.5f) : 0u;
    TIM8->CCR2 = mode == MOTOR_PWM ? (uint32_t)(duty[1] * 4200.0f + 0.5f) : 0u;
    TIM8->CCR3 = mode == MOTOR_PWM ? (uint32_t)(duty[2] * 4200.0f + 0.5f) : 0u;
    pending_mode = mode;
    ready = true;
    return !inhibited;
}

bool bsp_motor_update(void)
{
    TIM8->SR = ~TIM_SR_UIF;
    if ((TIM8->CR1 & TIM_CR1_DIR) || TIM8->CNT > 600u ||
        (!ready && motor_mode != MOTOR_OFF)) {
        bsp_motor_off(); return false;
    }
    /* A priority-0 fault may interrupt the priority-1 FOC write. Never let
       its resumed/stale preload re-enable gates after an emergency stop. */
    if (inhibited) { ready = false; return true; }
    if (ready) {
        if (pending_mode != motor_mode) {
            unsigned mode = pending_mode;
            if (mode == MOTOR_OFF) bsp_motor_off();
            else if (mode == MOTOR_PRECHARGE) {
                GPIOA->BSRR = GPIO_PIN_7;
                GPIOB->BSRR = GPIO_PIN_0 | GPIO_PIN_1;
            } else {
                /* GPIO precharge lows must fall before any high-side AF is exposed. */
                GPIOA->BSRR = GPIO_PIN_7 << 16;
                GPIOB->BSRR = (GPIO_PIN_0 | GPIO_PIN_1) << 16;
                uint32_t deadtime = DWT->CYCCNT;
                while (DWT->CYCCNT - deadtime < 100u) {}
                TIM8->CCER |= GATE_CHANNELS;
                GPIOA->MODER = (GPIOA->MODER & ~(3u << 14)) | (2u << 14);
                GPIOB->MODER = (GPIOB->MODER & ~15u) | 10u;
                GPIOC->MODER = (GPIOC->MODER & ~(63u << 12)) | (42u << 12);
            }
            motor_mode = mode;
        }
        /* Report quantized CCR/ARR, not the unrounded floating command. */
        motor_duty[0] = motor_mode == MOTOR_PWM ? (float)TIM8->CCR1 / 4200.0f : 0.0f;
        motor_duty[1] = motor_mode == MOTOR_PWM ? (float)TIM8->CCR2 / 4200.0f : 0.0f;
        motor_duty[2] = motor_mode == MOTOR_PWM ? (float)TIM8->CCR3 / 4200.0f : 0.0f;
    }
    ready = false;
    return true;
}

bool bsp_motor_sample_begin(void)
{
    uint32_t now = DWT->CYCCNT;
    uint32_t period = now - last_sample;
    bool valid = !last_sample || (period >= 8000u && period <= 8800u);
    if (last_sample) {
        if (period < motor_period_min) motor_period_min = period;
        if (period > motor_period_max) motor_period_max = period;
    }
    last_sample = sample_start = now;
    ++motor_cycles;
    return valid;
}

void bsp_motor_sample_end(void)
{
    uint32_t elapsed = DWT->CYCCNT - sample_start;
    if (elapsed > motor_work_max) motor_work_max = elapsed;
}

bool bsp_motor_load(foc_calibration_t *calibration)
{
    const record_t *r = (const record_t *)0x080e0000u;
    if (!record_valid(r)) return false;
    *calibration = r->cal;
    return true;
}

bool bsp_motor_save(const foc_calibration_t *calibration)
{
    if (motor_mode != MOTOR_OFF || (TIM8->CR1 & TIM_CR1_CEN)) return false;
    record_t r = {.version = 1u, .poles = 7u, .cal = *calibration, .magic = 0x464f4331u};
    r.checksum = checksum(&r);
    FLASH_EraseInitTypeDef erase = {.TypeErase = FLASH_TYPEERASE_SECTORS,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3, .Sector = FLASH_SECTOR_11, .NbSectors = 1u};
    uint32_t failed;
    if (HAL_FLASH_Unlock() != HAL_OK) return false;
    bool ok = HAL_FLASHEx_Erase(&erase, &failed) == HAL_OK;
    for (unsigned i = 0; ok && i < sizeof r; i += 4u) {
        uint32_t word;
        memcpy(&word, (const uint8_t *)&r + i, sizeof word);
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, 0x080e0000u + i, word) == HAL_OK;
    }
    HAL_FLASH_Lock();
    foc_calibration_t readback;
    return ok && bsp_motor_load(&readback) && memcmp(&readback, calibration, sizeof readback) == 0;
}
