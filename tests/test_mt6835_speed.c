/*
 * 测速算法的宿主验证：用合成角度数据喂给真实驱动，检查速度算得对不对。
 *
 * 为什么需要它：板子上电机轴是静止的，实测速度恒为 0，验证不了任何东西。
 * 这里用一个假 port（假 SPI + 假时间戳）造出"以已知角速度旋转"的数据流，
 * 就能确定性地验证：
 *   1. 匀速正转时速度值 = 理论值
 *   2. 反转符号正确
 *   3. 过零（角度从 0x1FFFFF 回绕到 0）不会算出巨大假速度
 *   4. 方向取反后速度符号跟着翻
 *   5. 一阶低通的收敛行为符合预期
 *
 * 编译（纯宿主，不碰 HAL）：
 *   gcc -std=c11 -Wall -Wextra -O2 -I App/Hardware/mt6835 \
 *       test_speed_host.c mt6835.c -lm -o test
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "mt6835.h"

/* 严格 -std=c11 下 math.h 不提供 M_PI */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* 假 port                                                             */
/* ------------------------------------------------------------------ */
static uint32_t g_angle;        /* 当前角度计数 */
static int32_t  g_step;         /* 每次读取增加多少计数 */
static uint32_t g_ts_us;        /* 假时间戳（微秒） */
static uint8_t  g_regs[256];    /* 假寄存器堆，给建链探针用 */

static void fake_cs(void *ctx, bool sel) { (void)ctx; (void)sel; }

static mt6835_status_t fake_transfer(void *ctx, const uint8_t *tx,
                                     uint8_t *rx, uint16_t len)
{
    (void)ctx;

    if (len == MT6835_FRAME_BYTES) {          /* 突发读角度 */
        rx[0] = tx[0];
        rx[1] = tx[1];
        rx[2] = (uint8_t)((g_angle >> 13) & 0xFFu);
        rx[3] = (uint8_t)((g_angle >> 5) & 0xFFu);
        rx[4] = (uint8_t)(((g_angle << 3) & 0xFFu));   /* status = 0 */
        rx[5] = mt6835_crc8(g_angle, 0u);

        g_angle = (g_angle + g_step) & MT6835_CPR_MASK;   /* 模拟转动 */
        return MT6835_OK;
    }

    if (len == MT6835_REG_FRAME_BYTES) {      /* 寄存器读写，给探针用 */
        uint16_t cmd  = (uint16_t)(((uint16_t)tx[0] << 8) | tx[1]);
        uint8_t  op   = (uint8_t)((cmd >> 12) & 0x0Fu);
        uint16_t addr = (uint16_t)(cmd & 0x0FFFu);

        if (op == MT6835_OP_READ) {
            rx[2] = g_regs[addr & 0xFFu];
        } else {
            g_regs[addr & 0xFFu] = tx[2];
            rx[2] = MT6835_ACK;
        }
        return MT6835_OK;
    }

    return MT6835_ERR_BUS;
}

static void     fake_delay_us(void *ctx, uint32_t us) { (void)ctx; (void)us; }
static void     fake_delay_ms(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }
static uint32_t fake_ts(void *ctx) { (void)ctx; return g_ts_us; }

/* ------------------------------------------------------------------ */
static int failures = 0;

static void check(const char *what, double got, double want, double tol)
{
    const double err = fabs(got - want);
    if (err > tol) {
        printf("  FAIL %-46s got %10.5f  want %10.5f  (tol %.5f)\n",
               what, got, want, tol);
        failures++;
    } else {
        printf("  ok   %-46s %10.5f\n", what, got);
    }
}

/* 每样本 50 us = 20 kHz */
#define DT_US       50u
#define FS_HZ       20000.0f

/* 多少计数对应多少 rad/s */
static double counts_to_rad_s(double counts_per_sample)
{
    return counts_per_sample * (2.0 * M_PI / 2097152.0) / (DT_US * 1e-6);
}

static void run(mt6835_t *dev, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        g_ts_us += DT_US;
        (void)mt6835_read(dev);
    }
}

int main(void)
{
    mt6835_port_t port;
    mt6835_t      dev;

    memset(&port, 0, sizeof(port));
    memset(g_regs, 0, sizeof(g_regs));
    port.cs_select    = fake_cs;
    port.transfer     = fake_transfer;
    port.delay_us     = fake_delay_us;
    port.delay_ms     = fake_delay_ms;
    port.timestamp_us = fake_ts;

    /* ---------------- 1. 匀速正转，滤波关掉（α=1）看原值 ---------------- */
    printf("[1] 匀速正转 100 counts/样本 @20kHz，α=1（不滤波）\n");
    g_angle = 100000u; g_step = 100; g_ts_us = 0u;
    if (mt6835_init(&dev, &port) != MT6835_OK) {
        printf("  FAIL 建链失败\n");
        return 1;
    }
    mt6835_set_speed_filter_alpha(&dev, 1.0f);
    run(&dev, 50);
    check("速度 rad/s", mt6835_speed_rad_s(&dev), counts_to_rad_s(100.0), 1e-3);
    check("速度 rpm",   mt6835_speed_rpm(&dev),
          counts_to_rad_s(100.0) * 60.0 / (2.0 * M_PI), 1e-2);

    /* ---------------- 2. 反转 ---------------- */
    printf("[2] 反转 100 counts/样本\n");
    g_angle = 1000000u; g_step = -100;
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_alpha(&dev, 1.0f);
    run(&dev, 50);
    check("速度 rad/s（应为负）", mt6835_speed_rad_s(&dev),
          -counts_to_rad_s(100.0), 1e-3);

    /* ---------------- 3. 过零不产生假速度 ---------------- */
    printf("[3] 过零：从 0x1FFFF0 起正转，必然跨过 0\n");
    g_angle = 0x1FFFF0u; g_step = 200;
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_alpha(&dev, 1.0f);

    {
        double worst = 0.0;
        int    i;
        const double expect = counts_to_rad_s(200.0);
        for (i = 0; i < 200; ++i) {
            double v;
            g_ts_us += DT_US;
            (void)mt6835_read(&dev);
            v = fabs(mt6835_speed_rad_s(&dev));
            if (v > worst) worst = v;
        }
        /* 过零那几拍若直接用原始值相减，会算出半圈/50us 的荒唐值（约 6e4 rad/s）。
           正确展开后所有样本都应等于理论值。 */
        check("全程最大 |速度| = 理论值", worst, expect, 1e-3);
    }

    /* ---------------- 4. 静止 ---------------- */
    printf("[4] 静止不动\n");
    g_angle = 555555u; g_step = 0;
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_alpha(&dev, 1.0f);
    run(&dev, 50);
    check("速度 rad/s", mt6835_speed_rad_s(&dev), 0.0, 1e-9);

    /* ---------------- 5. 方向取反 ---------------- */
    printf("[5] dir_invert 后速度符号应翻转\n");
    g_angle = 100000u; g_step = 100;
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_alpha(&dev, 1.0f);
    mt6835_set_direction(&dev, true);
    run(&dev, 50);
    check("速度 rad/s（应为负）", mt6835_speed_rad_s(&dev),
          -counts_to_rad_s(100.0), 1e-3);

    /* ---------------- 6. 低通收敛 ---------------- */
    printf("[6] 一阶低通收敛性（α=0.05, 400 拍 ≈ 20ms > 5τ）\n");
    g_angle = 100000u; g_step = 500;
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_alpha(&dev, 0.05f);
    run(&dev, 400);
    /* 一阶 IIR 稳态值 = 输入值，5τ 后误差 < 1% */
    check("滤波后速度 rad/s", mt6835_speed_rad_s(&dev),
          counts_to_rad_s(500.0), counts_to_rad_s(500.0) * 0.01);
    check("未滤波原值", mt6835_speed_raw_rad_s(&dev),
          counts_to_rad_s(500.0), 1e-3);

    /* ---------------- 7. 按截止频率配置 ---------------- */
    printf("[7] set_speed_filter_hz(200Hz, 20kHz) -> α = 1-exp(-2π*0.01)\n");
    mt6835_init(&dev, &port);
    mt6835_set_speed_filter_hz(&dev, 200.0f, 20000.0f);
    check("α", (double)dev.speed_alpha,
          1.0 - exp(-2.0 * M_PI * 200.0 / 20000.0), 1e-6);

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "ALL PASS" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
