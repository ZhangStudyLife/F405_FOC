/*
 * 一次性宿主校验：只编译 mt6835.c 的纯逻辑部分（CRC-8 / 帧解包）。
 * 不进固件构建，也不进仓库跟踪（tmp/ 已在 .gitignore 的本地分析产物范畴）。
 *
 * 断言值来源：MT6835 数据手册的 CRC 定义（多项式 0x07、初值 0x00、无最终异或，
 * 覆盖 24 bit = angle21 + status3），手算校验见下方注释。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mt6835.h"

static int failures = 0;

static void check_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-38s got 0x%08X want 0x%08X\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %-38s 0x%08X\n", what, got);
    }
}

/* 独立的参考实现：直接按字节走的 CRC-8，用于交叉验证 mt6835_crc8 的分字节写法 */
static uint8_t ref_crc8(const uint8_t *bytes, size_t n)
{
    uint8_t crc = 0x00;
    size_t  i;
    int     k;

    for (i = 0; i < n; ++i) {
        crc ^= bytes[i];
        for (k = 0; k < 8; ++k) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

int main(void)
{
    /* ---- 1. CRC-8 已知值（手算） ---- */
    printf("[1] CRC-8 known values\n");
    /* angle=0, status=0 -> 三个数据字节全 0 -> CRC = 0x00 */
    check_u32("crc8(0x000000, 0)", mt6835_crc8(0x000000u, 0u), 0x00u);

    /* angle=0x1FFFFF, status=7 -> 字节 FF FF FF -> 逐位展开得 0x0F */
    check_u32("crc8(0x1FFFFF, 7)", mt6835_crc8(0x1FFFFFu, 7u), 0x0Fu);

    /* ---- 2. 与逐字节参考实现交叉验证（穷举一批角度/状态组合） ---- */
    printf("[2] cross-check vs byte-wise reference\n");
    {
        uint32_t angle;
        uint8_t  status;
        int      mismatches = 0;

        for (angle = 0u; angle <= 0x1FFFFFu; angle += 9973u) {
            for (status = 0u; status < 8u; ++status) {
                uint8_t bytes[3];
                bytes[0] = (uint8_t)((angle >> 13) & 0xFFu);
                bytes[1] = (uint8_t)((angle >> 5) & 0xFFu);
                bytes[2] = (uint8_t)(((angle << 3) & 0xFFu) | status);
                if (mt6835_crc8(angle, status) != ref_crc8(bytes, 3)) {
                    mismatches++;
                }
            }
        }
        /* 0x1FFFFF / 9973 = 210 -> 211 个角度 x 8 个状态 */
        check_u32("mismatches (expect 0)", (uint32_t)mismatches, 0u);
    }

    /* ---- 3. 突发帧解包：角度/状态/CRC 必须落在协议规定的字节位置 ---- */
    printf("[3] burst frame decode\n");
    {
        /* 构造一个自洽的帧：angle 已知，反推 CRC，再走解包路径 */
        const uint32_t angle  = 0x0ABCDEu;   /* 21 bit 以内 */
        const uint8_t  status = 0x05u;
        uint8_t        rx[MT6835_FRAME_BYTES];

        rx[0] = 0xA0u;   /* 命令回显 */
        rx[1] = 0x03u;
        rx[2] = (uint8_t)((angle >> 13) & 0xFFu);
        rx[3] = (uint8_t)((angle >> 5) & 0xFFu);
        rx[4] = (uint8_t)(((angle << 3) & 0xFFu) | status);
        rx[5] = mt6835_crc8(angle, status);

        check_u32("decode_angle", mt6835_decode_angle(rx), angle);
        check_u32("decode_status", (uint32_t)mt6835_decode_status(rx), status);
        check_u32("frame crc matches", mt6835_crc8(mt6835_decode_angle(rx),
                                                   mt6835_decode_status(rx)), rx[5]);
    }

    /* ---- 4. 边界：全 1 角度、全 0 角度 ---- */
    printf("[4] boundary angles\n");
    {
        uint8_t rx[MT6835_FRAME_BYTES];
        memset(rx, 0, sizeof(rx));
        rx[2] = 0xFFu; rx[3] = 0xFFu; rx[4] = 0xFFu;
        check_u32("decode all-ones -> 21 bit max", mt6835_decode_angle(rx), 0x1FFFFFu);
        check_u32("decode all-ones status", (uint32_t)mt6835_decode_status(rx), 7u);

        memset(rx, 0, sizeof(rx));
        check_u32("decode all-zeros", mt6835_decode_angle(rx), 0u);
        check_u32("decode all-zeros status", (uint32_t)mt6835_decode_status(rx), 0u);
    }

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "ALL PASS" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
