/**
  ******************************************************************************
  * @file    scope_encoder.c
  * @brief   编码器 -> RAM 示波器 的绑定层。
  *
  * 通道布局、放大系数、示波器实例全部收在本文件里，main.c 一行都不用知道。
  * 本文件不含任何 HAL / CMSIS / STM32 头文件，可直接在 PC 上编译做单元测试。
  ******************************************************************************
  */

#include "scope_encoder.h"
#include "scope.h"

/* -------------------------------------------------------------------------- */
/* 通道布局                                                                    */
/* -------------------------------------------------------------------------- */

#define ENC_CH_POS      0u   /**< 位置，毫度 */
#define ENC_CH_SPD      1u   /**< 速度，rpm x100（已低通） */
#define ENC_CH_SPD_RAW  2u   /**< 速度，rpm x100（未滤波） */
#define ENC_CH_RAW      3u   /**< 原始 21 bit 计数 */
#define ENC_CH_CRC      4u   /**< 芯片给的 CRC */
#define ENC_CH_STATUS   5u   /**< 传感器状态位 */

#define ENC_CH_COUNT    6u

#if (SCOPE_CHANNELS < ENC_CH_COUNT)
#error "scope_encoder 需要 SCOPE_CHANNELS >= 6（见 scope_encoder.h 的通道布局）"
#endif

/* -------------------------------------------------------------------------- */
/* 放大系数                                                                    */
/* -------------------------------------------------------------------------- */

/* 示波器通道是 int32，浮点必须放大成定点存；上位机用 --div 除回来。
   位置放大 1000 倍（毫度）是因为 21 bit 编码器的角度分辨率约 1.7e-4 度，
   直接取整会把分辨率丢光。 */
#define ENC_POS_SCALE   1000.0f
#define ENC_SPD_SCALE   100.0f
#define ENC_RAD_TO_RPM  9.549296585513721f   /* 60 / (2*pi) */

/* -------------------------------------------------------------------------- */
/* 示波器实例                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * 放在本文件里、文件作用域静态 —— 它是本模块的实现细节，不是对外接口。
 * 上位机 tools/scope_gui.py 靠符号名 s_scope 找到它。
 *
 * 注意：如果 main.c 里没有调用 scope_encoder_init/update，链接时
 * --gc-sections 会把本模块整个丢掉，符号也就不存在 —— 这正是我们想要的：
 * 不用示波器时，Flash 和 RAM 一分都不占。
 */
static scope_t s_scope;

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

void scope_encoder_init(const mt6835_t *dev, uint32_t sample_rate_hz)
{
    (void)dev;   /* 目前不需要保存实例，布局对单编码器是固定的 */
    scope_init(&s_scope, sample_rate_hz);
}

void scope_encoder_update(const mt6835_t *dev)
{
    int32_t sv[SCOPE_CHANNELS];
    uint32_t i;

    if (dev == NULL) {
        return;
    }

    sv[ENC_CH_POS]     = (int32_t)(mt6835_angle_deg(dev) * ENC_POS_SCALE);
    sv[ENC_CH_SPD]     = (int32_t)(mt6835_speed_rpm(dev) * ENC_SPD_SCALE);
    sv[ENC_CH_SPD_RAW] = (int32_t)(mt6835_speed_raw_rad_s(dev)
                                   * ENC_RAD_TO_RPM * ENC_SPD_SCALE);
    sv[ENC_CH_RAW]     = (int32_t)mt6835_raw(dev);
    sv[ENC_CH_CRC]     = (int32_t)dev->sample.crc;
    sv[ENC_CH_STATUS]  = (int32_t)mt6835_status(dev);

    /* SCOPE_CHANNELS 可能大于 6（用户自己加了通道），多出来的补 0 */
    for (i = ENC_CH_COUNT; i < (uint32_t)SCOPE_CHANNELS; ++i) {
        sv[i] = 0;
    }

    scope_push(&s_scope, sv);
}

void scope_encoder_freeze(void)
{
    scope_freeze(&s_scope);
}

void scope_encoder_resume(void)
{
    scope_resume(&s_scope);
}

bool scope_encoder_is_frozen(void)
{
    return scope_is_frozen(&s_scope);
}

uint32_t scope_encoder_total(void)
{
    return scope_total_samples(&s_scope);
}
