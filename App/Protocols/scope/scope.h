/**
  ******************************************************************************
  * @file    scope.h
  * @brief   RAM 示波器：在中断里把信号写进环形缓冲，主机通过 SWD 整块读走。
  *
  * ---------------------------------------------------------------------------
  * 为什么需要它
  * ---------------------------------------------------------------------------
  * 这块板子只有 SWD，没有串口，OpenOCD 对当前 ST-Link 也只支持 hla_swd
  * （HLA 驱动不支持 SWO），所以既不能 printf 也不能用 ITM 实时输出。
  *
  * 但"实时输出"其实不是看波形的最佳方式 —— SWD 单次读变量要几毫秒，
  * 顶多采到几百 Hz，而 FOC 是 20 kHz。正确做法是**反过来**：
  * 目标板自己以全速采样写进 RAM，主机事后把整块内存一次性搬走。
  * 这样时间分辨率是真正的 20 kHz，而不是被调试链路限死。
  *
  * ---------------------------------------------------------------------------
  * 用法
  * ---------------------------------------------------------------------------
  *   // 1) 定义实例（放全局，主机靠符号名找它）
  *   scope_t g_scope;
  *
  *   // 2) 初始化一次
  *   scope_init(&g_scope, 20000u);        // 20 kHz
  *
  *   // 3) 在控制中断里推样本（约 25 周期，可忽略）
  *   int32_t v[SCOPE_CHANNELS] = { ia, ib, angle, id, iq, duty };
  *   scope_push(&g_scope, v);
  *
  *   // 4) 出故障时冻结，保留触发前的历史
  *   if (fault) scope_freeze(&g_scope);
  *
  * 然后跑 `python tools/scope_capture.py` 就能看到波形。
  *
  * ---------------------------------------------------------------------------
  * 内存
  * ---------------------------------------------------------------------------
  * SCOPE_DEPTH * SCOPE_CHANNELS * 4 字节 + 32 字节头部。
  * 默认 1024 * 6 * 4 = 24 KB（F405 有 128 KB，当前固件只用了 2 KB）。
  * 不要的话把 SCOPE_ENABLE 置 0，或在 CMake 里改小这两个宏。
  ******************************************************************************
  */

#ifndef APP_PROTOCOLS_SCOPE_SCOPE_H
#define APP_PROTOCOLS_SCOPE_SCOPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* 配置（可用 CMake 的 target_compile_definitions 覆盖）                        */
/* ========================================================================== */

/** @brief 每样本几路信号。 */
#ifndef SCOPE_CHANNELS
#define SCOPE_CHANNELS 6u
#endif

/** @brief 环形缓冲深度（样本数），**必须是 2 的幂**，回绕用位与实现。 */
#ifndef SCOPE_DEPTH
#define SCOPE_DEPTH 1024u
#endif

#define SCOPE_MAGIC    0x53434F50u   /* "SCOP" */
#define SCOPE_VERSION  1u

/**
  * @brief 头部字段个数（8 个 uint32，共 32 字节）。
  * @note  主机端 tools/scope_capture.py 按这个布局解析。
  *        改这里的字段顺序/数量，必须同步改主机脚本并递增 SCOPE_VERSION。
  */
#define SCOPE_HEADER_WORDS 8u

/* ========================================================================== */
/* 类型                                                                        */
/* ========================================================================== */

/**
  * @brief 示波器缓冲。
  * @note  头部全部是 volatile uint32，**没有任何位域或指针**，
  *        保证主机端能用固定的 32 字节布局解析，不会踩到对齐填充的坑。
  */
typedef struct {
    volatile uint32_t magic;            /**< 'SCOP'，主机用来确认符号找对了 */
    volatile uint32_t version;          /**< 布局版本 */
    volatile uint32_t channels;         /**< = SCOPE_CHANNELS */
    volatile uint32_t depth;            /**< = SCOPE_DEPTH，环形长度 */
    volatile uint32_t sample_rate_hz;   /**< 采样率，主机用来标时间轴 */
    volatile uint32_t write_index;      /**< 环形写指针（已写样本数，不回绕） */
    volatile uint32_t total_samples;    /**< 累计样本数，用于判断是否被覆盖过 */
    volatile uint32_t frozen;           /**< 非 0 = 已冻结，停止记录 */

    int32_t data[SCOPE_DEPTH * SCOPE_CHANNELS];  /**< 样本区，逐样本交错存储 */
} scope_t;

/* ========================================================================== */
/* 接口                                                                        */
/* ========================================================================== */

/**
  * @brief  初始化并把数据区清零。
  * @param  sample_rate_hz 采样率，仅作为元数据告诉主机，本模块不用它
  * @note   在主循环里调用一次。清零 24 KB 大约几百微秒，不要在中断里做。
  */
void scope_init(scope_t *s, uint32_t sample_rate_hz);

/**
  * @brief  推进一个样本。**可从中断调用**。
  * @param  values 长度至少 SCOPE_CHANNELS 的数组，按通道顺序排列
  * @note   约 25 个周期（6 次 store + 少量算术），对 20 kHz 控制环无影响。
  *         已冻结时直接返回，所以 freeze 之后缓冲内容保持不变。
  */
void scope_push(scope_t *s, const int32_t *values);

/**
  * @brief  把 float 数组按 scale 转成定点后推进（免去调用方手写取整）。
  * @param  scale 例如 1000.0f 表示保留 3 位小数
  */
void scope_push_scaled(scope_t *s, const float *values, float scale);

/**
  * @brief  冻结：停止记录，缓冲里保留的是**触发前**最近的 SCOPE_DEPTH 个样本。
  * @note   这就是"预触发捕获"。在故障分支里调一次，就能回看故障瞬间前后的波形。
  */
void scope_freeze(scope_t *s);

/** @brief 解冻，继续记录。 */
void scope_resume(scope_t *s);

/** @brief 是否已冻结。 */
bool scope_is_frozen(const scope_t *s);

/** @brief 累计样本数。 */
uint32_t scope_total_samples(const scope_t *s);

/**
  * @brief  把环形缓冲整理成按时间递增的线性序列（最老的样本在前）。
  * @param  out         输出缓冲，至少要 SCOPE_CHANNELS * depth 个 int32
  * @param  max_samples 输出最多容纳多少个样本
  * @retval 实际写出的样本数
  * @note   主机端也会做同样的整理；这个函数是给板内自检/单元测试用的。
  */
uint32_t scope_linearize(const scope_t *s, int32_t *out, uint32_t max_samples);

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTOCOLS_SCOPE_SCOPE_H */
