/**
  ******************************************************************************
  * @file    mt6835.h
  * @brief   MT6835（MagnTek / 纳芯微，21-bit 磁编码器）SPI 驱动。
  *
  * ---------------------------------------------------------------------------
  * 设计约束（见 App/README.md 的分层规则）
  * ---------------------------------------------------------------------------
  *  1. 本文件与 mt6835.c **不包含任何 HAL / CMSIS / STM32 头文件**。
  *     所有板级依赖通过 mt6835_port_t 里的函数指针注入（Port/Adapter 模式）。
  *     好处：驱动可以在 PC 上编译做单元测试；换 MCU、换 SPI 外设、换片选脚
  *     都只改一个板级 port 文件，驱动本体一行不动。
  *
  *  2. 驱动只管"和芯片说话"，不管"控制电机"。电角度换算、零点对齐属于 FOC 层。
  *
  * ---------------------------------------------------------------------------
  * 通信协议要点（数据手册 Rev 1.3 + SimpleFOC 官方驱动交叉核对）
  * ---------------------------------------------------------------------------
  *  - SPI 模式 3（CPOL=1, CPHA=1），MSB first，SCK 上限 16 MHz、推荐 8 MHz。
  *    本工程 SPI3 = PCLK1(42 MHz)/4 = 10.5 MHz，在规格内。
  *
  *  - **突发读（快路径，48 bit = 6 字节）**：一次片选拿全 角度 + 状态 + CRC。
  *        发: A0 03 00 00 00 00
  *        收: [0][1] 命令回显  [2] angle[20:13]  [3] angle[12:5]
  *            [4] (angle[4:0] << 3) | status[2:0]   [5] CRC-8
  *    20 kHz 控制环每周期只需要这一次传输。
  *
  *  - **寄存器访问（慢路径，24 bit = 3 字节）**：
  *        命令字 = (op << 12) | (addr & 0x0FFF)，第 3 字节为数据。
  *    只能在非实时上下文调用（配置、校准），不要在控制中断里用。
  *
  *  - **CRC-8**：多项式 0x07，初值 0x00，无最终异或；覆盖 24 bit
  *    （angle21 + status3），即突发帧的 [2][3][4] 三字节。
  ******************************************************************************
  */

#ifndef APP_HARDWARE_MT6835_MT6835_H
#define APP_HARDWARE_MT6835_MT6835_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL：stdint.h 不提供，freestanding 下由 stddef.h 提供 */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* 编译期开关（可用 CMake 的 target_compile_definitions 覆盖）                  */
/* ========================================================================== */

/** 是否编译进 CRC 校验代码。0 可省下每帧约 30~40 个周期，但失去数据完整性保护。 */
#ifndef MT6835_ENABLE_CRC
#define MT6835_ENABLE_CRC 1
#endif

/** 上电后等待芯片内部就绪的时间。此期间 SPI 不应答，读会失败。 */
#ifndef MT6835_POWER_ON_DELAY_MS
#define MT6835_POWER_ON_DELAY_MS 20u
#endif

/** 慢路径两帧之间片选保持高电平的最短时间。快路径天然满足，不插延时。 */
#ifndef MT6835_TCSH_MIN_US
#define MT6835_TCSH_MIN_US 1u
#endif

/** mt6835_init() 建链重试次数。 */
#ifndef MT6835_INIT_RETRY
#define MT6835_INIT_RETRY 5u
#endif

/* ========================================================================== */
/* 常量                                                                        */
/* ========================================================================== */

#define MT6835_ANGLE_BITS      21u
#define MT6835_CPR             (1u << MT6835_ANGLE_BITS)   /* 2097152 counts/圈 */
#define MT6835_HALF_CPR        (1u << (MT6835_ANGLE_BITS - 1u))
#define MT6835_CPR_MASK        (MT6835_CPR - 1u)

#define MT6835_FRAME_BYTES     6u   /* 突发读帧长 */
#define MT6835_REG_FRAME_BYTES 3u   /* 寄存器访问帧长 */

#define MT6835_TWO_PI          6.28318530717958647692f
#define MT6835_RAD_PER_COUNT   (MT6835_TWO_PI / (float)MT6835_CPR)

/* 传感器状态位（突发帧第 5 字节的低 3 位） */
#define MT6835_ST_OVERSPEED    0x01u  /**< 转速超限 */
#define MT6835_ST_WEAK_FIELD   0x02u  /**< 磁场过弱：磁钢掉了/气隙太大 */
#define MT6835_ST_UNDERVOLT    0x04u  /**< 供电欠压 */

/* SPI 操作码（命令字高 4 位） */
#define MT6835_OP_READ         0x3u   /**< 读寄存器 */
#define MT6835_OP_ZERO         0x5u   /**< 以当前位置为零点 */
#define MT6835_OP_WRITE        0x6u   /**< 写寄存器 */
#define MT6835_OP_BURST        0xAu   /**< 突发读角度（含状态与 CRC） */
#define MT6835_OP_PROG         0xCu   /**< 烧写 EEPROM */

/** 写/烧写/置零成功的应答值。 */
#define MT6835_ACK             0x55u

/**
  * @brief 建链探针写入 0x001 的测试值。
  * @note  0x001 是"客户保留"寄存器，用它做写入-回读测试不会碰到任何功能位。
  */
#define MT6835_PROBE_PATTERN   0x5Au

/* 寄存器地址（12 bit）—— 与数据手册第 10 章"寄存器表"逐条核对过 */
#define MT6835_REG_USERID      0x001u   /* 客户保留 EEPROM 寄存器，**不是器件 ID** */
#define MT6835_REG_RESERVED    0x002u   /* Not Used */
#define MT6835_REG_ANGLE1      0x003u
#define MT6835_REG_ANGLE2      0x004u
#define MT6835_REG_ANGLE3      0x005u
#define MT6835_REG_ANGLE4      0x006u
#define MT6835_REG_ABZ_RES1    0x007u
#define MT6835_REG_ABZ_RES2    0x008u
#define MT6835_REG_ZERO1       0x009u
#define MT6835_REG_ZERO2       0x00Au   /* 与 OPTS0 共用同一地址，仅位域不同 */
#define MT6835_REG_OPTS1       0x00Bu
#define MT6835_REG_OPTS2       0x00Cu
#define MT6835_REG_OPTS3       0x00Du
#define MT6835_REG_OPTS4       0x00Eu
#define MT6835_REG_OPTS5       0x011u
#define MT6835_REG_NLC_BASE    0x013u   /* 非线性校准表，192 字节 */
#define MT6835_REG_CAL_STATUS  0x113u

/* ========================================================================== */
/* 类型                                                                        */
/* ========================================================================== */

/** @brief 驱动返回码。0 为成功。 */
typedef enum {
    MT6835_OK = 0,
    MT6835_ERR_PARAM,       /**< 入参非法 */
    MT6835_ERR_NOT_INIT,    /**< 尚未建链成功 */
    MT6835_ERR_BUS,         /**< 总线错误（超时之外的传输失败） */
    MT6835_ERR_TIMEOUT,     /**< 传输超时：芯片不应答 */
    MT6835_ERR_CRC,         /**< CRC 校验失败，本次采样已丢弃 */
    MT6835_ERR_NACK         /**< 写/烧写没有收到 0x55 应答 */
} mt6835_status_t;

/**
  * @brief  板级传输接口。由板级 port 文件实现（本工程见 mt6835_port_stm32.c）。
  * @note   ctx 由驱动原样透传，port 用它找回自己的上下文。
  */
typedef struct {
    void *ctx;   /**< 板级上下文指针 */

    /**
      * @brief  片选控制。
      * @param  selected true = 选中芯片（CSN 拉低），false = 释放
      * @note   驱动保证任何返回路径上都会调用一次 selected=false。
      */
    void (*cs_select)(void *ctx, bool selected);

    /**
      * @brief  全双工收发一整帧（片选已在低电平）。
      * @retval MT6835_OK 或 mt6835_status_t 错误码
      */
    mt6835_status_t (*transfer)(void *ctx,
                                const uint8_t *tx,
                                uint8_t       *rx,
                                uint16_t       len);

    /** @brief 微秒级忙等。可为 NULL（驱动会退化为不延时）。 */
    void (*delay_us)(void *ctx, uint32_t us);

    /** @brief 毫秒级忙等。可为 NULL（建链时就不做上电等待与重试间隔）。 */
    void (*delay_ms)(void *ctx, uint32_t ms);
} mt6835_port_t;

/** @brief 一次有效采样。 */
typedef struct {
    uint32_t raw;      /**< 21 bit 原始角度计数值 */
    uint8_t  status;   /**< 传感器状态位，见 MT6835_ST_* */
    uint8_t  crc;      /**< 芯片发来的 CRC-8 原值（便于离线比对） */
} mt6835_sample_t;

/** @brief 驱动实例。静态分配，不使用堆。 */
typedef struct {
    mt6835_port_t   port;            /**< 板级接口副本 */

    mt6835_sample_t sample;          /**< 最近一次**有效**采样 */
    uint8_t         user_id;         /**< 0x001 的原值（客户保留寄存器，非器件 ID） */

    int64_t         total_counts;    /**< 多圈累计计数（已应用方向） */
    uint32_t        prev_raw;        /**< 上一次原始值，用于多圈展开 */

    uint32_t        zero_offset;     /**< 零位偏移（counts，21 bit 有效） */
    bool            dir_invert;      /**< 方向取反 */
    bool            check_crc;       /**< 运行时 CRC 开关，初值取自 MT6835_ENABLE_CRC */
    bool            have_sample;     /**< 是否已经有过一次有效采样 */
    bool            crc_fault;       /**< 最近一次读取是否 CRC 失败 */
    bool            initialized;     /**< 建链是否成功 */

    uint32_t        comm_errors;     /**< 传输失败累计 */
    uint32_t        crc_errors;      /**< CRC 失败累计 */
} mt6835_t;

/* ========================================================================== */
/* 生命周期                                                                    */
/* ========================================================================== */

/**
  * @brief  初始化并建链。
  * @param  port 板级接口；驱动会**拷贝**一份，因此可以是栈上临时量。
  * @retval MT6835_OK           建链成功，且已完成第一次有效采样
  * @retval MT6835_ERR_PARAM    port 或其必填回调为空
  * @retval MT6835_ERR_BUS / MT6835_ERR_TIMEOUT 芯片无应答（未上电、接线、片选脚错）
  * @note   会阻塞 MT6835_POWER_ON_DELAY_MS + 重试间隔，只能在 main() 初始化阶段调用。
  *         失败时**不会**调用 Error_Handler()：控制工程里让 MCU 关中断死循环
  *         是不可接受的，请由调用方决定降级策略。
  */
mt6835_status_t mt6835_init(mt6835_t *dev, const mt6835_port_t *port);

/**
  * @brief  建链探针：验证 SPI 双向通信是否真的通。
  * @param  user_id 可为 NULL；非空时回填 0x001 的**原值**
  * @retval MT6835_OK         芯片在，且读写都通
  * @retval MT6835_ERR_BUS    回读值不等于写入值（芯片没在应答）
  *
  * @note   **MT6835 没有器件 ID 寄存器。** 0x001 在数据手册第 10 章里注明是
  *         "客户保留寄存器(EEPROM)"，出厂未编程时读回 0x00 —— 所以**绝不能**
  *         拿它的值判断链路好坏（这样会把一颗正常的新芯片判成故障）。
  *
  *         这里改用"写入 → 回读 → 恢复"来证明双向通信：往 0x001 写
  *         MT6835_PROBE_PATTERN，回读比对，再把原值写回。
  *         写进去的只是 RAM 影子，只有显式发 0xC000 才会落进 EEPROM，
  *         因此这个测试是非破坏性的。
  */
mt6835_status_t mt6835_probe(mt6835_t *dev, uint8_t *user_id);

/* ========================================================================== */
/* 快路径：读角度（可从中断调用）                                               */
/* ========================================================================== */

/**
  * @brief  读一次角度。一次片选、一帧 48 bit，拿到 角度 + 状态 + CRC。
  * @retval MT6835_OK          采样已更新
  * @retval MT6835_ERR_CRC     CRC 失败：**保留上一次有效采样**，crc_errors++
  * @retval 其他               传输失败：保留上一次有效采样，comm_errors++
  * @note   这是为 20 kHz 控制环准备的唯一 I/O 入口。
  *         上板实测（Release，10.5 MHz，真实数据）单次约 **17.0 us**，
  *         其中线上时间只有 4.57 us，其余是 HAL 逐字节轮询开销。
  *         占 50 us 控制周期的 34%，优化方向见 App/README.md 第 4 节。
  *         调用前请确保已 mt6835_init() 成功。
  */
mt6835_status_t mt6835_read(mt6835_t *dev);

/* ========================================================================== */
/* 取值（纯计算，无 I/O，ISR 安全）                                             */
/* ========================================================================== */

/** @brief 芯片原始 21 bit 计数值（未做零位/方向处理）。 */
uint32_t mt6835_raw(const mt6835_t *dev);

/** @brief 机械角，弧度，[0, 2π)。已应用零位偏移与方向。 */
float mt6835_angle_rad(const mt6835_t *dev);

/** @brief 机械角，角度制，[0, 360)。 */
float mt6835_angle_deg(const mt6835_t *dev);

/** @brief 多圈累计计数（已应用方向）。一圈 = MT6835_CPR。 */
int64_t mt6835_total_counts(const mt6835_t *dev);

/** @brief 多圈展开后的总角度，弧度，可超出 [0, 2π)。 */
float mt6835_full_angle_rad(const mt6835_t *dev);

/** @brief 已完成的整圈数（截断取整）。 */
int32_t mt6835_turns(const mt6835_t *dev);

/** @brief 最近一次有效采样的传感器状态位，见 MT6835_ST_*。 */
uint8_t mt6835_status(const mt6835_t *dev);

/** @brief 最近一次读取是否 CRC 失败。 */
bool mt6835_crc_fault(const mt6835_t *dev);

/**
  * @brief  综合健康判据：已建链 且 最近一次读取无 CRC 错误 且 传感器无告警位。
  * @note   适合直接作为 FOC 的使能条件——磁场丢失时 MT6835_ST_WEAK_FIELD 会置位，
  *         此时角度不可信，必须停机而不是继续闭环。
  */
bool mt6835_healthy(const mt6835_t *dev);

/** @brief 读取累计错误计数。任一参数可为 NULL。 */
void mt6835_error_counts(const mt6835_t *dev, uint32_t *comm_errors, uint32_t *crc_errors);

/** @brief 0x001（客户保留寄存器）的原值，建链探针读取时缓存。 */
uint8_t mt6835_user_id(const mt6835_t *dev);

/* ========================================================================== */
/* 配置（慢路径，禁止在控制中断中调用）                                          */
/* ========================================================================== */

/**
  * @brief  读一个 8 bit 寄存器（24 bit 帧）。
  */
mt6835_status_t mt6835_read_reg(mt6835_t *dev, uint16_t addr, uint8_t *val);

/**
  * @brief  写一个 8 bit 寄存器。
  * @note   数据手册第 10 章只对 **0xC000 烧录 EEPROM** 和 **0x5000 置零** 定义了
  *         0x55 应答码；普通写寄存器（0x6）**没有**应答。因此这里只校验传输本身
  *         成功，不要求 0x55。需要确认写入生效请用 mt6835_read_reg() 回读。
  */
mt6835_status_t mt6835_write_reg(mt6835_t *dev, uint16_t addr, uint8_t val);

/**
  * @brief  把当前角度写进零点寄存器 ZERO_POS[11:0]（0x5000 命令）。
  * @note   数据手册 7.6.7：该命令**只写寄存器，不烧 EEPROM**。掉电要保留，
  *         需在 ≥1 ms 后再调用 mt6835_store_eeprom()。
  */
mt6835_status_t mt6835_set_zero_here(mt6835_t *dev);

/**
  * @brief  把所有寄存器刷入 EEPROM（0xC000 命令）。
  * @warning 成功后需等 ≥ 6 秒再断电。属于破坏性操作。
  */
mt6835_status_t mt6835_store_eeprom(mt6835_t *dev);

/**
  * @brief  设置软件零位偏移（不动芯片 EEPROM，只影响驱动输出）。
  * @param  raw 期望被当作 0 度的原始计数值
  * @note   这是把编码器零点对齐到电机电角度零点的推荐做法：不写芯片，
  *         不给 Flash 寿命找麻烦，标定结果由上层保存。
  */
void mt6835_set_zero_offset(mt6835_t *dev, uint32_t raw);

/** @brief 读取当前软件零位偏移。 */
uint32_t mt6835_get_zero_offset(const mt6835_t *dev);

/** @brief 方向取反。用于编码器计数方向与电机正转方向相反时。 */
void mt6835_set_direction(mt6835_t *dev, bool invert);

/** @brief 查询方向是否取反。 */
bool mt6835_get_direction(const mt6835_t *dev);

/** @brief 运行时开关 CRC 校验。 */
void mt6835_enable_crc(mt6835_t *dev, bool enable);

/* ========================================================================== */
/* 纯函数（无状态，便于离线单测）                                               */
/* ========================================================================== */

/**
  * @brief  计算 MT6835 的 CRC-8。
  * @param  angle21 21 bit 角度
  * @param  status  3 bit 状态
  * @retval CRC-8（多项式 0x07，初值 0x00，无最终异或）
  */
uint8_t mt6835_crc8(uint32_t angle21, uint8_t status);

/**
  * @brief  由突发帧的 5 个字节解出 21 bit 角度。
  * @param  rx  至少 MT6835_FRAME_BYTES 字节的接收缓冲
  */
uint32_t mt6835_decode_angle(const uint8_t *rx);

/** @brief 由突发帧解出 3 bit 状态。 */
uint8_t mt6835_decode_status(const uint8_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* APP_HARDWARE_MT6835_MT6835_H */
