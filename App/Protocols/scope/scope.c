/**
  ******************************************************************************
  * @file    scope.c
  * @brief   RAM 示波器实现。
  *
  * 本文件**不包含任何 HAL / CMSIS / STM32 头文件**，纯逻辑，
  * 因此可以直接在 PC 上用 gcc 编译做单元测试（见 App/README.md 的分层规则）。
  ******************************************************************************
  */

#include "scope.h"

/* 深度必须是 2 的幂，回绕才可以用位与代替取模（取模在 M4 上要几十个周期，
   而这个函数是每 50 us 就要跑一次的）。编译期直接卡死，不留隐患。 */
#if (SCOPE_DEPTH == 0u) || ((SCOPE_DEPTH & (SCOPE_DEPTH - 1u)) != 0u)
#error "SCOPE_DEPTH 必须是 2 的幂"
#endif

#if (SCOPE_CHANNELS == 0u) || (SCOPE_CHANNELS > 16u)
#error "SCOPE_CHANNELS 必须在 1~16 之间"
#endif

void scope_init(scope_t *s, uint32_t sample_rate_hz)
{
    uint32_t i;
    const uint32_t n = (uint32_t)SCOPE_DEPTH * (uint32_t)SCOPE_CHANNELS;

    if (s == NULL) {
        return;
    }

    s->channels       = (uint32_t)SCOPE_CHANNELS;
    s->depth          = (uint32_t)SCOPE_DEPTH;
    s->sample_rate_hz = sample_rate_hz;
    s->write_index    = 0u;
    s->total_samples  = 0u;
    s->frozen         = 0u;

    for (i = 0u; i < n; ++i) {
        s->data[i] = 0;
    }

    /* magic / version 最后写：主机看到 magic 正确时，其余字段一定已经就位 */
    s->version = SCOPE_VERSION;
    s->magic   = SCOPE_MAGIC;
}

void scope_push(scope_t *s, const int32_t *values)
{
    uint32_t base;
    uint32_t c;

    if ((s == NULL) || (values == NULL) || (s->frozen != 0u)) {
        return;
    }

    base = (s->write_index & ((uint32_t)SCOPE_DEPTH - 1u)) * (uint32_t)SCOPE_CHANNELS;

    for (c = 0u; c < (uint32_t)SCOPE_CHANNELS; ++c) {
        s->data[base + c] = values[c];
    }

    /* 指针最后更新：主机若在写过程中读走，最多丢掉最新一个不完整的样本，
       不会读到"指针已前进但数据还是旧的"这种错位状态。 */
    s->write_index   = s->write_index + 1u;
    s->total_samples = s->total_samples + 1u;
}

void scope_push_scaled(scope_t *s, const float *values, float scale)
{
    int32_t tmp[SCOPE_CHANNELS];
    uint32_t c;

    if ((s == NULL) || (values == NULL)) {
        return;
    }

    for (c = 0u; c < (uint32_t)SCOPE_CHANNELS; ++c) {
        /* 加 0.5/-0.5 做四舍五入，再截断。比 roundf() 省一次库调用。 */
        const float v = values[c] * scale;
        tmp[c] = (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
    }

    scope_push(s, tmp);
}

void scope_freeze(scope_t *s)
{
    if (s != NULL) {
        s->frozen = 1u;
    }
}

void scope_resume(scope_t *s)
{
    if (s != NULL) {
        s->frozen = 0u;
    }
}

bool scope_is_frozen(const scope_t *s)
{
    return (s == NULL) ? false : (s->frozen != 0u);
}

uint32_t scope_total_samples(const scope_t *s)
{
    return (s == NULL) ? 0u : s->total_samples;
}

uint32_t scope_linearize(const scope_t *s, int32_t *out, uint32_t max_samples)
{
    uint32_t depth;
    uint32_t ch;
    uint32_t count;
    uint32_t start;
    uint32_t i;
    uint32_t k;

    if ((s == NULL) || (out == NULL)) {
        return 0u;
    }

    depth = s->depth;
    ch    = s->channels;

    if ((depth == 0u) || (ch == 0u)) {
        return 0u;
    }

    /* 可用的样本数：写满之前有多少算多少，写满之后就是一个整环 */
    count = (s->write_index < depth) ? s->write_index : depth;
    if (count > max_samples) {
        count = max_samples;
    }

    /* 最老的样本位置。整环时是下一次要覆盖的那一格，
       未写满时就是 0。 */
    start = (s->write_index >= depth) ? (s->write_index & (depth - 1u)) : 0u;

    for (i = 0u; i < count; ++i) {
        const uint32_t src = ((start + i) & (depth - 1u)) * ch;
        for (k = 0u; k < ch; ++k) {
            out[i * ch + k] = s->data[src + k];
        }
    }

    return count;
}
