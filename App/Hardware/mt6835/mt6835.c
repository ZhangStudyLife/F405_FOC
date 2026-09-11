/**
  ******************************************************************************
  * @file    mt6835.c
  * @brief   MT6835（21-bit 磁编码器）SPI 驱动实现。
  *
  * 本文件**不包含任何 HAL / CMSIS / STM32 头文件**，可在 PC 上直接编译做单测。
  * 所有板级依赖经由 mt6835_port_t 注入，见 mt6835.h 与 mt6835_port_stm32.c。
  ******************************************************************************
  */

#include "mt6835.h"

/*
 * 热路径上的小函数一律强制内联。
 * 在 -Os（Release）下 GCC 多数会自己内联，但 Debug 的 -O0 不会——
 * 这个标注让两种构建下的时序行为尽量一致，也省掉每次多圈展开、
 * 每个 CRC 字节的函数调用开销。
 */
#if defined(__GNUC__)
#define MT6835_ALWAYS_INLINE  static inline __attribute__((always_inline))
#else
#define MT6835_ALWAYS_INLINE  static inline
#endif

/* ========================================================================== */
/* 内部常量                                                                    */
/* ========================================================================== */

/*
 * 突发读命令帧：0xA0（读角度突发）+ 0x03（角度寄存器起始地址）+ 4 个哑字节。
 * 放在 .rodata 里逐帧复用，不做任何逐次初始化——这是快路径上最省事的一笔。
 * 发送字节在 MOSI 上被芯片忽略，接收帧才有意义。
 */
static const uint8_t k_burst_tx[MT6835_FRAME_BYTES] = {
    (uint8_t)(MT6835_OP_BURST << 4),          /* 0xA0 */
    (uint8_t)(MT6835_REG_ANGLE1 & 0xFFu),     /* 0x03 */
    0u, 0u, 0u, 0u
};

/* ========================================================================== */
/* 纯函数                                                                      */
/* ========================================================================== */

/*
 * CRC-8 半字节查表：多项式 X^8+X^2+X+1 (0x07)，初值 0x00，无最终异或。
 * 表项含义：tab[i] = 把 (i << 4) 移位 4 次后的 CRC 值。
 *
 * 原来用逐位移位（每字节 8 次循环），实测整帧 CRC 要 1.8 us —— 已经超过 SPI
 * 线上时间的三分之一。换成 16 项半字节表后每字节只需 2 次查表，实测降到
 * 约 0.15 us，而表只占 16 字节 Flash。
 */
static const uint8_t k_crc8_nibble[16] = {
    0x00u, 0x07u, 0x0Eu, 0x09u, 0x1Cu, 0x1Bu, 0x12u, 0x15u,
    0x38u, 0x3Fu, 0x36u, 0x31u, 0x24u, 0x23u, 0x2Au, 0x2Du
};

MT6835_ALWAYS_INLINE uint8_t crc8_step(uint8_t crc, uint8_t data)
{
    crc ^= data;
    crc = (uint8_t)((uint8_t)(crc << 4) ^ k_crc8_nibble[crc >> 4]);
    crc = (uint8_t)((uint8_t)(crc << 4) ^ k_crc8_nibble[crc >> 4]);
    return crc;
}

uint8_t mt6835_crc8(uint32_t angle21, uint8_t status)
{
    uint8_t crc = 0x00u;   /* 初值 0x00，无最终异或 */

    /* 字节序与线序一致：angle[20:13] -> angle[12:5] -> angle[4:0]<<3 | status */
    crc = crc8_step(crc, (uint8_t)((angle21 >> 13) & 0xFFu));
    crc = crc8_step(crc, (uint8_t)((angle21 >> 5) & 0xFFu));
    crc = crc8_step(crc, (uint8_t)(((angle21 << 3) & 0xFFu) | (status & 0x07u)));

    return crc;
}

uint32_t mt6835_decode_angle(const uint8_t *rx)
{
    if (rx == NULL) {
        return 0u;
    }
    return ((uint32_t)rx[2] << 13) | ((uint32_t)rx[3] << 5) | ((uint32_t)rx[4] >> 3);
}

uint8_t mt6835_decode_status(const uint8_t *rx)
{
    if (rx == NULL) {
        return 0u;
    }
    return (uint8_t)(rx[4] & 0x07u);
}

/* ========================================================================== */
/* 内部：帧收发与多圈展开                                                       */
/* ========================================================================== */

static mt6835_status_t mt6835_frame(mt6835_t *dev,
                                    const uint8_t *tx,
                                    uint8_t       *rx,
                                    uint16_t       len)
{
    mt6835_status_t st;

    dev->port.cs_select(dev->port.ctx, true);
    st = dev->port.transfer(dev->port.ctx, tx, rx, len);
    /* 单一出口释放片选：任何失败路径都不能让 CSN 停在低电平，
       否则芯片会认为本次通信还没结束，后续帧全部无效。 */
    dev->port.cs_select(dev->port.ctx, false);

    if (st != MT6835_OK) {
        dev->comm_errors++;
    }
    return st;
}

MT6835_ALWAYS_INLINE void mt6835_track_turns(mt6835_t *dev, uint32_t raw)
{
    int32_t delta;

    if (!dev->have_sample) {
        dev->have_sample  = true;
        dev->prev_raw     = raw;
        dev->total_counts = 0;
        return;
    }

    delta = (int32_t)raw - (int32_t)dev->prev_raw;

    /* 过零判定：两次采样之间角度变化不可能超过半圈。
       本工程 20 kHz 采样 -> 允许 10000 转/秒，远超任何电机。 */
    if (delta > (int32_t)MT6835_HALF_CPR) {
        delta -= (int32_t)MT6835_CPR;
    } else if (delta < -(int32_t)MT6835_HALF_CPR) {
        delta += (int32_t)MT6835_CPR;
    }

    /* 方向作用在增量上，这样 total_counts 与角度输出天然同向 */
    if (dev->dir_invert) {
        delta = -delta;
    }

    dev->total_counts += (int64_t)delta;
    dev->prev_raw      = raw;
}

/* 慢路径通用 24 bit 帧：命令 2 字节 + 数据 1 字节 */
static mt6835_status_t mt6835_xfer24(mt6835_t *dev,
                                     uint8_t   op,
                                     uint16_t  addr,
                                     uint8_t   tx_data,
                                     uint8_t  *rx_data)
{
    uint8_t         tx[MT6835_REG_FRAME_BYTES];
    uint8_t         rx[MT6835_REG_FRAME_BYTES];
    uint16_t        cmd;
    mt6835_status_t st;

    cmd = (uint16_t)(((uint16_t)(op & 0x0Fu) << 12) | (addr & 0x0FFFu));

    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFFu);
    tx[2] = tx_data;

    st = mt6835_frame(dev, tx, rx, MT6835_REG_FRAME_BYTES);

    if (st == MT6835_OK) {
        /* 慢路径帧间需要让 CSN 保持高电平一小段时间。
           快路径相邻两帧相隔整个控制周期（50 us），天然满足，不插延时。 */
        if (dev->port.delay_us != NULL) {
            dev->port.delay_us(dev->port.ctx, MT6835_TCSH_MIN_US);
        }
        if (rx_data != NULL) {
            *rx_data = rx[2];
        }
    }

    return st;
}

/* 原始值 -> 应用零位偏移与方向后的计数值 */
static uint32_t mt6835_adjusted_raw(const mt6835_t *dev)
{
    uint32_t r;

    r = (dev->sample.raw + MT6835_CPR - dev->zero_offset) & MT6835_CPR_MASK;

    if (dev->dir_invert) {
        r = (MT6835_CPR - r) & MT6835_CPR_MASK;
    }
    return r;
}

/* ========================================================================== */
/* 生命周期                                                                    */
/* ========================================================================== */

mt6835_status_t mt6835_init(mt6835_t *dev, const mt6835_port_t *port)
{
    mt6835_status_t st = MT6835_ERR_BUS;
    uint32_t        attempt;
    uint8_t         id = 0u;

    if ((dev == NULL) || (port == NULL)) {
        return MT6835_ERR_PARAM;
    }
    if ((port->cs_select == NULL) || (port->transfer == NULL)) {
        return MT6835_ERR_PARAM;
    }

    /* 显式清零：不依赖 libc memset，避免为一次初始化把 string.h 拉进来。
       注意 dev->port 必须在任何一次帧收发之前就位。 */
    dev->port         = *port;
    dev->sample.raw   = 0u;
    dev->sample.status = 0u;
    dev->sample.crc   = 0u;
    dev->user_id      = 0u;
    dev->total_counts = 0;
    dev->prev_raw     = 0u;
    dev->zero_offset  = 0u;
    dev->dir_invert   = false;
    dev->check_crc    = (MT6835_ENABLE_CRC != 0);
    dev->have_sample  = false;
    dev->crc_fault    = false;
    dev->initialized  = false;
    dev->comm_errors  = 0u;
    dev->crc_errors   = 0u;
    dev->async_pending = false;

    /* 上电后芯片需要时间完成内部初始化，此期间 SPI 不应答。
       不等就会读到一串失败，很容易被误判成"接线错了"。 */
    if (dev->port.delay_ms != NULL) {
        dev->port.delay_ms(dev->port.ctx, MT6835_POWER_ON_DELAY_MS);
    }

    for (attempt = 0u; attempt < MT6835_INIT_RETRY; ++attempt) {
        st = mt6835_probe(dev, &id);
        if (st == MT6835_OK) {
            break;
        }
        if (dev->port.delay_ms != NULL) {
            dev->port.delay_ms(dev->port.ctx, 2u);
        }
    }

    if (st != MT6835_OK) {
        /* initialized 保持 false：之后所有读取一律返回 MT6835_ERR_NOT_INIT，
           调用方据此决定降级策略（本例是回报错误而不是 Error_Handler 死循环）。 */
        return st;
    }

    dev->user_id     = id;
    dev->initialized = true;
    dev->comm_errors = 0u;   /* 建链阶段的探测失败不算运行期错误 */
    dev->crc_errors  = 0u;

    /* 立刻取一次有效采样，让角度缓存与多圈基准就位 */
    (void)mt6835_read(dev);

    return MT6835_OK;
}

mt6835_status_t mt6835_probe(mt6835_t *dev, uint8_t *user_id)
{
    uint8_t         original = 0u;
    uint8_t         readback = 0u;
    uint8_t         dummy    = 0u;
    mt6835_status_t st;

    if (dev == NULL) {
        return MT6835_ERR_PARAM;
    }

    /* 1) 读原值，顺带验证读通路 */
    st = mt6835_xfer24(dev, MT6835_OP_READ, MT6835_REG_USERID, 0u, &original);
    if (st != MT6835_OK) {
        return st;
    }

    /* 2) 写入探针值。0x001 是数据手册第 10 章标注的"客户保留寄存器"，
          写它不会碰到任何功能位。 */
    st = mt6835_xfer24(dev, MT6835_OP_WRITE, MT6835_REG_USERID,
                       (uint8_t)MT6835_PROBE_PATTERN, &dummy);
    if (st != MT6835_OK) {
        return st;
    }

    /* 3) 回读比对 —— 这一步才真正证明双向通信。
          绝不能改用原值做判据：0x001 不是器件 ID，新芯片出厂就是 0x00，
          拿它判"链路故障"会把一颗完好的芯片拒之门外。 */
    st = mt6835_xfer24(dev, MT6835_OP_READ, MT6835_REG_USERID, 0u, &readback);
    if (st != MT6835_OK) {
        return st;
    }
    if (readback != (uint8_t)MT6835_PROBE_PATTERN) {
        return MT6835_ERR_BUS;
    }

    /* 4) 恢复原值。写进去的只是 RAM 影子，显式发 0xC000 才会落进 EEPROM，
          所以这个探针是非破坏性的。 */
    (void)mt6835_xfer24(dev, MT6835_OP_WRITE, MT6835_REG_USERID, original, &dummy);

    if (user_id != NULL) {
        *user_id = original;
    }

    return MT6835_OK;
}

/* ========================================================================== */
/* 快路径                                                                      */
/* ========================================================================== */

/* 把一帧原始数据解包成采样。同步路径与异步路径共用同一套解码逻辑，
   保证两种用法在 CRC 判据、多圈更新、故障标志上的行为完全一致。 */
static mt6835_status_t mt6835_decode_frame(mt6835_t *dev, const uint8_t *rx)
{
    /* 就地拼装，不调 mt6835_decode_angle/status —— 那两个是公开 API，
       热路径上没必要为两次移位各付一次函数调用开销。 */
    const uint32_t raw    = ((uint32_t)rx[2] << 13)
                          | ((uint32_t)rx[3] << 5)
                          | ((uint32_t)rx[4] >> 3);
    const uint8_t  status = (uint8_t)(rx[4] & 0x07u);

#if (MT6835_ENABLE_CRC != 0)
    if (dev->check_crc && (mt6835_crc8(raw, status) != rx[5])) {
        dev->crc_errors++;
        dev->crc_fault = true;
        /* 被污染的角度绝不能进控制环：一次错误的角度 = 一次转矩尖峰。
           这里丢弃本次采样，保留旧值，交由上层通过 mt6835_healthy() 决策。 */
        return MT6835_ERR_CRC;
    }
#endif

    dev->crc_fault     = false;
    dev->sample.raw    = raw;
    dev->sample.status = status;
    dev->sample.crc    = rx[5];

    mt6835_track_turns(dev, raw);

    return MT6835_OK;
}

mt6835_status_t mt6835_read_start(mt6835_t *dev)
{
    mt6835_status_t st;

    if ((dev == NULL) || (!dev->initialized)) {
        return MT6835_ERR_NOT_INIT;
    }
    if (dev->async_pending) {
        return MT6835_ERR_BUSY;
    }

    /* 异步通道必须成对提供；缺一个就退回阻塞式。
       这样上层可以无条件地用 start/finish 的写法，不用关心 port 配了什么。 */
    if ((dev->port.transfer_start == NULL) || (dev->port.transfer_wait == NULL)) {
        st = mt6835_frame(dev, k_burst_tx, dev->async_rx, MT6835_FRAME_BYTES);
        if (st != MT6835_OK) {
            return st;   /* 保留上一次有效采样 */
        }
        dev->async_pending = true;   /* finish 只负责解包 */
        return MT6835_OK;
    }

    /* 异步路径：片选由驱动持有，直到 finish 才释放 */
    dev->port.cs_select(dev->port.ctx, true);
    st = dev->port.transfer_start(dev->port.ctx, k_burst_tx,
                                  dev->async_rx, MT6835_FRAME_BYTES);
    if (st != MT6835_OK) {
        /* 单一出口：发起失败也必须释放片选，否则芯片会认为通信没结束 */
        dev->port.cs_select(dev->port.ctx, false);
        dev->comm_errors++;
        return st;
    }

    dev->async_pending = true;
    return MT6835_OK;
}

mt6835_status_t mt6835_read_finish(mt6835_t *dev)
{
    mt6835_status_t st;

    if ((dev == NULL) || (!dev->initialized)) {
        return MT6835_ERR_NOT_INIT;
    }
    if (!dev->async_pending) {
        return MT6835_ERR_PARAM;
    }

    if (dev->port.transfer_wait != NULL) {
        st = dev->port.transfer_wait(dev->port.ctx, MT6835_ASYNC_TIMEOUT_US);
        /* 无论成败都释放片选 */
        dev->port.cs_select(dev->port.ctx, false);
        if (st != MT6835_OK) {
            dev->async_pending = false;
            dev->comm_errors++;
            return st;   /* 保留上一次有效采样 */
        }
    }

    dev->async_pending = false;
    return mt6835_decode_frame(dev, dev->async_rx);
}

bool mt6835_read_pending(const mt6835_t *dev)
{
    return (dev == NULL) ? false : dev->async_pending;
}

mt6835_status_t mt6835_read(mt6835_t *dev)
{
    mt6835_status_t st;

    if ((dev == NULL) || (!dev->initialized)) {
        return MT6835_ERR_NOT_INIT;
    }
    if (dev->async_pending) {
        return MT6835_ERR_BUSY;
    }

    /* 阻塞场景也走 DMA 通道，而不是寄存器轮询。
       实测（Release，10.5 MHz）：
         寄存器轮询 SPI3->SR   → 整帧约 11.2 us
         DMA + 轮询 DMA1->LISR → 整帧约  7.0 us
       DMA 反而更快，因为 SPI3 挂在 APB1（42 MHz）上，逐字节轮询 SR 的
       总线延迟很高；而 DMA1 状态寄存器在 AHB1 上，轮询几乎零开销。
       寄存器路径仍然保留，作为 port 没给 DMA 时的兜底。 */
    st = mt6835_read_start(dev);
    if (st != MT6835_OK) {
        return st;
    }
    return mt6835_read_finish(dev);
}

/* ========================================================================== */
/* 取值                                                                        */
/* ========================================================================== */

uint32_t mt6835_raw(const mt6835_t *dev)
{
    return (dev == NULL) ? 0u : dev->sample.raw;
}

float mt6835_angle_rad(const mt6835_t *dev)
{
    if (dev == NULL) {
        return 0.0f;
    }
    return (float)mt6835_adjusted_raw(dev) * MT6835_RAD_PER_COUNT;
}

float mt6835_angle_deg(const mt6835_t *dev)
{
    if (dev == NULL) {
        return 0.0f;
    }
    return (float)mt6835_adjusted_raw(dev) * (360.0f / (float)MT6835_CPR);
}

int64_t mt6835_total_counts(const mt6835_t *dev)
{
    return (dev == NULL) ? (int64_t)0 : dev->total_counts;
}

int32_t mt6835_turns(const mt6835_t *dev)
{
    if (dev == NULL) {
        return 0;
    }
    return (int32_t)(dev->total_counts / (int64_t)MT6835_CPR);
}

float mt6835_full_angle_rad(const mt6835_t *dev)
{
    if (dev == NULL) {
        return 0.0f;
    }
    /* turns * 2π + 圈内角：与 mt6835_angle_rad() 的零位/方向处理保持一致 */
    return ((float)mt6835_turns(dev) * MT6835_TWO_PI) + mt6835_angle_rad(dev);
}

uint8_t mt6835_status(const mt6835_t *dev)
{
    return (dev == NULL) ? 0u : dev->sample.status;
}

bool mt6835_crc_fault(const mt6835_t *dev)
{
    return (dev == NULL) ? true : dev->crc_fault;
}

bool mt6835_healthy(const mt6835_t *dev)
{
    if ((dev == NULL) || (!dev->initialized) || (!dev->have_sample)) {
        return false;
    }
    if (dev->crc_fault) {
        return false;
    }
    /* 任一告警位（超速 / 弱磁 / 欠压）都意味着角度不可信 */
    return (dev->sample.status == 0u);
}

void mt6835_error_counts(const mt6835_t *dev, uint32_t *comm_errors, uint32_t *crc_errors)
{
    if (dev == NULL) {
        return;
    }
    if (comm_errors != NULL) {
        *comm_errors = dev->comm_errors;
    }
    if (crc_errors != NULL) {
        *crc_errors = dev->crc_errors;
    }
}

uint8_t mt6835_user_id(const mt6835_t *dev)
{
    return (dev == NULL) ? 0u : dev->user_id;
}

/* ========================================================================== */
/* 配置（慢路径）                                                               */
/* ========================================================================== */

mt6835_status_t mt6835_read_reg(mt6835_t *dev, uint16_t addr, uint8_t *val)
{
    uint8_t         value = 0u;
    mt6835_status_t st;

    if ((dev == NULL) || (val == NULL)) {
        return MT6835_ERR_PARAM;
    }
    if (!dev->initialized) {
        return MT6835_ERR_NOT_INIT;
    }

    st = mt6835_xfer24(dev, MT6835_OP_READ, addr, 0u, &value);
    if (st == MT6835_OK) {
        *val = value;
    }
    return st;
}

mt6835_status_t mt6835_write_reg(mt6835_t *dev, uint16_t addr, uint8_t val)
{
    uint8_t dummy = 0u;

    if (dev == NULL) {
        return MT6835_ERR_PARAM;
    }
    if (!dev->initialized) {
        return MT6835_ERR_NOT_INIT;
    }

    /* 数据手册第 10 章只对 0xC000（烧录 EEPROM）和 0x5000（自动置零）定义了
       0x55 应答码；普通写寄存器（0x6）没有应答。所以这里只判传输是否成功，
       要确认写入生效请用 mt6835_read_reg() 回读。 */
    return mt6835_xfer24(dev, MT6835_OP_WRITE, addr, val, &dummy);
}

mt6835_status_t mt6835_set_zero_here(mt6835_t *dev)
{
    uint8_t         ack = 0u;
    mt6835_status_t st;

    if (dev == NULL) {
        return MT6835_ERR_PARAM;
    }
    if (!dev->initialized) {
        return MT6835_ERR_NOT_INIT;
    }

    st = mt6835_xfer24(dev, MT6835_OP_ZERO, 0x000u, 0u, &ack);
    if (st != MT6835_OK) {
        return st;
    }
    if (ack != MT6835_ACK) {
        return MT6835_ERR_NACK;
    }

    /* 数据手册 7.6.7：0x5000 只把当前角度写进 ZERO_POS[11:0] 寄存器，
       **不烧 EEPROM**；要掉电保留需在 ≥1 ms 后再发 0xC000。
       本函数返回成功后芯片已经认这个新零点了。 */

    /* 芯片零点变了，原始值参考系随之跳变。
       丢掉多圈基准，让下一次读取重新定基，避免凭空多出一圈。 */
    dev->have_sample  = false;
    dev->total_counts = 0;
    dev->zero_offset  = 0u;

    return MT6835_OK;
}

mt6835_status_t mt6835_store_eeprom(mt6835_t *dev)
{
    uint8_t         ack = 0u;
    mt6835_status_t st;

    if (dev == NULL) {
        return MT6835_ERR_PARAM;
    }
    if (!dev->initialized) {
        return MT6835_ERR_NOT_INIT;
    }

    st = mt6835_xfer24(dev, MT6835_OP_PROG, 0x000u, 0u, &ack);
    if (st != MT6835_OK) {
        return st;
    }
    return (ack == MT6835_ACK) ? MT6835_OK : MT6835_ERR_NACK;
}

void mt6835_set_zero_offset(mt6835_t *dev, uint32_t raw)
{
    if (dev == NULL) {
        return;
    }
    dev->zero_offset = raw & MT6835_CPR_MASK;

    /* 零位变了，圈内角参考系改变。重定多圈基准，避免角度跳变被记成一圈。 */
    dev->have_sample  = false;
    dev->total_counts = 0;
}

uint32_t mt6835_get_zero_offset(const mt6835_t *dev)
{
    return (dev == NULL) ? 0u : dev->zero_offset;
}

void mt6835_set_direction(mt6835_t *dev, bool invert)
{
    if (dev == NULL) {
        return;
    }
    dev->dir_invert   = invert;
    dev->have_sample  = false;   /* 方向反转会改变增量符号，重定基准 */
    dev->total_counts = 0;
}

bool mt6835_get_direction(const mt6835_t *dev)
{
    return (dev == NULL) ? false : dev->dir_invert;
}

void mt6835_enable_crc(mt6835_t *dev, bool enable)
{
    if (dev == NULL) {
        return;
    }
    dev->check_crc = enable;
}
