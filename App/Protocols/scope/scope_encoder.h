/**
  ******************************************************************************
  * @file    scope_encoder.h
  * @brief   把编码器的位置/速度录进 RAM 示波器，供上位机经 SWD 抓取。
  *
  * ---------------------------------------------------------------------------
  * 为什么要单独一层
  * ---------------------------------------------------------------------------
  * 示波器本身（App/Protocols/scope）只认"往环形缓冲塞几个 int32"，
  * 它不该知道 MT6835，也不该知道位置要乘 1000、速度要乘 100。
  * 那些都属于"业务怎么用示波器"，堆在 main.c 里就是一堆通道数组和魔数。
  *
  * 所以把这一层绑定的活儿全部收进本模块：
  *   - 通道布局（哪一路是位置、哪一路是速度）
  *   - 放大系数（浮点 -> int32 定点）
  *   - 示波器实例本身
  * main.c 只需要一次 init + 每拍一次 update，看不到任何示波器细节。
  *
  * ---------------------------------------------------------------------------
  * 通道布局（主机端 tools/scope_gui.py 的默认名与此对应）
  * ---------------------------------------------------------------------------
  *   ch0  位置      毫度      [0, 360000)      除数 1000 -> 度
  *   ch1  速度      rpm x100  已低通           除数 100  -> rpm
  *   ch2  速度      rpm x100  未滤波（看噪声）  除数 100  -> rpm
  *   ch3  原始角度  21 bit 计数
  *   ch4  芯片 CRC
  *   ch5  传感器状态位（bit0 超速 / bit1 弱磁 / bit2 欠压）
  *
  * 需要 SCOPE_CHANNELS >= 6，否则编译期直接报错（见 scope_encoder.c）。
  ******************************************************************************
  */

#ifndef APP_PROTOCOLS_SCOPE_SCOPE_ENCODER_H
#define APP_PROTOCOLS_SCOPE_SCOPE_ENCODER_H

#include <stdint.h>

#include "mt6835.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  初始化示波器缓冲。
  * @param  dev            编码器实例（只用来标识，模块不保存指针）
  * @param  sample_rate_hz 采样率，即 update() 的调用频率。仅作为元数据
  *                        告诉上位机用来标时间轴，本模块不做任何定时。
  * @note   会清零整个缓冲（默认 24 KB），在主循环里调一次，别在中断里调。
  */
void scope_encoder_init(const mt6835_t *dev, uint32_t sample_rate_hz);

/**
  * @brief  录一个样本。**可从中断调用**，约 25 个周期。
  * @note   每次 mt6835_read() 成功之后调一次即可。
  *         缓冲被冻结（scope_freeze）时本函数直接返回，不影响控制环。
  */
void scope_encoder_update(const mt6835_t *dev);

/**
  * @brief  冻结 / 解冻记录（保留触发前的历史，做预触发捕获）。
  * @note   出故障时调 scope_encoder_freeze()，缓冲里留下的就是故障前的波形。
  */
void scope_encoder_freeze(void);
void scope_encoder_resume(void);
bool scope_encoder_is_frozen(void);

/** @brief 已录样本总数，可用于确认采集是否在推进。 */
uint32_t scope_encoder_total(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTOCOLS_SCOPE_SCOPE_ENCODER_H */
