# App —— 业务代码层

本目录是本工程的**业务代码**，与 CubeMX 生成的 `Core/`、`Drivers/` 分开管理。
目标是：驱动和 FOC 算法都能被复用、能被单测、换硬件时改动局部化。

---

## 1. 分层与依赖方向

依赖是**单向**的，任何时候都不允许反向或跨层回指：

```
        ┌───────────────┐        ┌──────────────────┐
        │   App/FOC/    │        │ App/Protocols/   │
        │  电机控制算法  │        │  通信协议        │
        │  纯逻辑        │        │  纯逻辑          │
        └───────┬───────┘        └────────┬─────────┘
                │                         │
                └───────────┬─────────────┘
                            ▼
                 ┌──────────────────────┐
                 │   App/Hardware/      │
                 │  驱动 + 板级适配      │
                 └──────────┬───────────┘
                            ▼
                 ┌──────────────────────┐
                 │ App/Hardware/bsp/    │  ← HAL 封装层
                 └──────────┬───────────┘
                            ▼
                 ┌──────────────────────┐
                 │  STM32 HAL / CMSIS   │
                 └──────────────────────┘
```

### 三条硬规则

| # | 规则 | 为什么 |
|---|---|---|
| 1 | `App/FOC/`、`App/Protocols/` **不得** include `stm32f4xx_hal.h` / `main.h` / `spi.h` 等任何 ST 头文件 | 算法与硬件解耦，才能在 PC 上验证、换芯片不改算法 |
| 2 | 驱动本体（如 `mt6835.c/.h`）**不得** include HAL；板级依赖通过函数指针接口注入 | 驱动可离线单测，换总线/换 MCU 只改一个 port 文件 |
| 3 | 整个 `App/` 中**只有** `Hardware/bsp/*.c` 和 `*_port_stm32.*` 允许出现 HAL 类型 | 把"ST 依赖"收敛到两个已知位置，边界清晰可检查 |

规则 3 可以用一条命令自查：

```bash
grep -rl "stm32f4xx_hal\|stm32f4xx\.h\|main\.h\"" App/ \
  | grep -v "App/Hardware/bsp/" | grep -v "_port_stm32"
# 输出为空 = 分层没被破坏
```

---

## 2. 目录职责

```
App/
├── README.md                       本文件
├── Hardware/
│   ├── bsp/                        HAL 封装层（唯一持有 HAL 的通用层）
│   │   ├── bsp_gpio.h/.c           输出引脚抽象：active_low 语义 + BSRR 直写
│   │   ├── bsp_spi.h/.c            SPI 总线抽象：阻塞收发 + 错误码转译 + 状态机自愈
│   │   └── bsp_time.h/.c           DWT 周期计数 / µs 时戳 / 关中断可用的忙等延时
│   └── mt6835/                     MT6835 21-bit 磁编码器
│       ├── mt6835.h/.c             驱动本体（协议、解包、CRC、多圈）—— 无 HAL
│       ├── mt6835_port_stm32.h/.c  板级适配：绑定 SPI3 + PA0，唯一知道接线的文件
│       └── README.md               协议细节、时序坑、实测数据、排障表
├── Protocols/                      预留：CRC 库、上位机帧协议
└── FOC/                            预留：Clarke/Park/SVPWM、电流环、速度环
```

---

## 3. 核心模式：Port / Adapter（依赖倒置）

驱动**定义它需要什么能力**，而不是依赖某个具体外设。以 MT6835 为例：

```
mt6835.h  定义 mt6835_port_t { cs_select, transfer, delay_us, delay_ms }
                    ▲                              ▲
                    │ 实现                          │ 使用
   mt6835_port_stm32.c                        mt6835.c
   （唯一 include HAL）                        （零 HAL 依赖）
                    │
                    ▼
       bsp_spi / bsp_gpio / bsp_time
```

带来三个具体好处：

1. **可单测**：`mt6835.c` 在 PC 上用 gcc 直接编译（本仓库已实测），
   把 `transfer` 换成一个查表桩函数就能验证解包和 CRC。
2. **可复用**：换成 MT6826S / AS5047 / 另一颗 SPI 编码器时，新写一个
   `xxx.c` + 一个 port 文件即可，`bsp_*` 和上层都不用动。
3. **可换硬件**：SPI3 改成 SPI1、片选从 PA0 改到 PB6，只改 `mt6835_port_stm32.c`
   和 main.c 里的一行调用。

### 命名约定

| 前缀 | 含义 | 示例 |
|---|---|---|
| `bsp_` | HAL 封装层 | `bsp_spi_transfer` |
| `mt6835_` | MT6835 驱动 | `mt6835_read` |
| `xxx_port_stm32_` | 板级适配入口 | `mt6835_port_stm32_bind` |
| 类型 `xxx_t` | 结构体 | `mt6835_t` |
| 枚举 `xxx_status_t` | 返回码，0 = 成功 | `MT6835_OK` |

头文件保护宏统一 `APP_<目录>_<文件>_H`。

---

## 4. 20 kHz 实时性：SPI 效率策略

### 实测时间预算（周期 50 µs）

**上板实测数据（Release，SPI3 = 10.5 MHz）—— 与设计估算差距很大，以实测为准：**

| 项 | 实测 | 说明 |
|---|---|---|
| SPI3 时钟 | 10.5 MHz | PCLK1 42 MHz ÷ 4，芯片上限 16 MHz、推荐 8 MHz |
| 48 bit 线上时间 | **4.57 µs**（768 周期） | 6 字节突发帧，物理下限 |
| HAL + 分发 + CRC 开销 | **约 12 µs**（约 2017 周期） | 明显高于原先估算的 1.5~2.5 µs |
| **单次 `mt6835_read()` 合计** | **17.0 µs**（2861 周期） | 占控制周期 **33%** |

> 测量方法：`bsp_time_cycles()` 包夹 `mt6835_read()`，结果存进 `g_mt6835_read_ns`。
> 时基本身已用墙钟做过交叉验证（DWT 计数值与实际经过时间一致），数据可信。
> 计时不含 `HAL_Delay`，是纯粹的单次读取耗时。

**结论：能跑，但代价偏高。** 20 kHz 周期 50 µs，一次编码器读取吃掉 1/3，
留给 Clarke/Park/PI/SVPWM 的只剩 33 µs（5544 周期）——够用，但毫无余量。

开销的大头不是 SPI 线上时间，而是 **HAL 逐字节轮询**：
`HAL_SPI_TransmitReceive` 对每个字节都要走一次
`SPI_WaitFlagStateUntilTimeout`（内部还调 `HAL_GetTick()`），6 字节就是 12 次等待。
线上时间 4.57 µs 反而只占三成。

### 已经做的优化

1. **一周期只读一次**。用 `0xA0` 突发帧一次拿全 角度 + 状态 + CRC，
   而不是"读角度 + 读状态"两次片选。
2. **不做双采样测速**。速度用带时戳的历史差分或观测器算，比多读一次 SPI 划算得多。
3. **命令帧放 `.rodata`**（`k_burst_tx`），每帧直接复用，零初始化开销。
4. **片选走 BSRR 直写**，不是 `HAL_GPIO_WritePin`（后者是读改写）。
5. **10.5 MHz 不动**。往上到 21 MHz（prescaler 2）**超出芯片 16 MHz 上限**，不要试。

### 下一步的三个选项（按性价比排序）

实测 17.0 µs 已超过原来 8 µs 的预算线，因此需要做一次取舍：

| 方案 | 预期效果 | 代价 | 风险 |
|---|---|---|---|
| **A. BSP 内做寄存器级快路径**<br>`bsp_spi_transfer_fast()` | 17.0 µs → **约 5~6 µs**（省掉全部 HAL 轮询） | 约 30 行 | 低。仍封装在 `bsp_spi.c` 内，分层不破 |
| **B. SPI DMA 异步** | 阻塞时间 → **约 2~3 µs**（4.57 µs 线上时间藏进 FOC 运算） | 改 `.ioc` + 新增回调 + 1 周期角度延迟 | 中。需重新生成 CubeMX 代码 |
| **C. 16-bit 数据宽度** | 17.0 µs → 约 11~12 µs（等待次数减半） | 运行时改 `hspi3.Init.DataSize` 并重调 `HAL_SPI_Init` | 中。引入 2 字节对齐约束 |

> A 和 B 可以叠加，叠加后单次读取接近 3 µs，占控制周期 6%。

### DMA 升级路径（选 B 时）

1. CubeMX 里给 SPI3 勾上 TX/RX 的 DMA（`.ioc` → SPI3 → DMA Settings）。
2. 在 `mt6835_port_t` **尾部追加**可选字段（向后兼容，老 port 不用改）：
   ```c
   mt6835_status_t (*transfer_start)(void *ctx, const uint8_t *tx, uint8_t *rx, uint16_t len);
   mt6835_status_t (*transfer_wait)(void *ctx, uint32_t timeout_us);
   ```
3. 在 `mt6835_read()` 里按"`transfer_start` 非空则走异步"分支。
   注意：DMA 传输期间片选**必须保持低电平**，直到 `transfer_wait` 返回。

---

## 5. 实测数据

### 5.1 SPI 读取耗时

| 构建 | 单次 `mt6835_read()` | 备注 |
|---|---|---|
| **Release** | **17.0 µs**（2861 周期） | 上板实测，观测变量 `g_mt6835_read_ns` |
| Debug | 未测（预期更差） | `-O0`，只用于功能调试 |

测量方法见 `../.claude/skills/foc-build-flash/SKILL.md` 的寄存器级调试章节：
`g_mt6835_read_ns`、`g_mt6835_loops`、`g_mt6835_raw`。

> **Debug 构建的实时性能没有参考价值**：`CMAKE_C_FLAGS_DEBUG = -O0 -g3`。
> 评估实时性只能用 Release。

### 5.2 上板联调状态

| 检查项 | 结果 |
|---|---|
| SPI3 配置（CR1=0x34F） | ✓ Master / Mode 3 / 8-bit / 10.5 MHz / SSM+SSI |
| GPIOC 引脚复用 | ✓ PC10/PC11/PC12 = AF6 (SPI3) |
| SPI3 外设时钟 | ✓ APB1ENR.SPI3EN = 1 |
| 片选 PA0 | ✓ 推挽输出，空闲高，拉低可测到电平变化 |
| 建链 | ✓ 成功（写-回读探针通过） |
| 连续读取 | ✓ 约 19500 次读取，`comm_errors = 0`、`crc_errors = 0` |
| 数据有效性 | ✓ 实测两帧：`raw=518786 → CRC=142`、`raw=518792 → CRC=57`，本驱动的 CRC-8 与芯片输出**逐位一致** |
| 传感器状态位 | ✓ `STATUS = 0`（无超速/弱磁/欠压告警） |
| 单次读取耗时 | 17.0 µs（Release，2861 周期） |

> **联调教训（已修）**：MT6835 **没有器件 ID 寄存器**。0x001 是数据手册第 10 章标注的
> "客户保留寄存器(EEPROM)"，出厂默认 `0x00`。早期版本的建链探针把"读到 `0x00`"当成
> 链路故障，于是一颗完好的芯片被反复判为"无应答"，白排查了两轮接线。
> 现在改用"写 `0x5A` → 回读比对 → 恢复原值"来验证双向通信。
> 详见 `Hardware/mt6835/README.md` 第 5 节。

---

## 6. CubeMX 重新生成的安全边界

改 `.ioc` 后重新生成代码时，**哪些文件会被覆盖**：

| 路径 | 重新生成会覆盖？ | 说明 |
|---|---|---|
| `Core/Src/*.c`、`Core/Inc/*.h` | 会 | 但 `/* USER CODE BEGIN */ ... /* USER CODE END */` 之间的内容会保留 |
| `cmake/stm32cubemx/CMakeLists.txt` | **会** | ⚠️ 绝对不要手工改这个文件 |
| `CMakeLists.txt`（顶层） | **不会** | 文件头写明只生成一次，用户源文件/包含路径加在这里 |
| `App/` 下的一切 | **不会** | 本目录完全由手工维护 |
| `.ioc.original.bak` | — | 已在 `.gitignore` 中忽略 |

结论：**所有自己的代码都放 `App/`**；`main.c` 里只允许在 `USER CODE` 块内写代码。

---

## 7. FOC 接入契约（草案，尚未实现）

给 `App/FOC/` 的接口设计定个方向。**由 FOC 层拥有接口，由 Hardware 层实现**，
依赖方向才不会反。

```c
/* App/FOC/foc_hw.h —— 纯函数指针，不含任何 ST 头文件 */
typedef struct {
    void *ctx;

    /* 一次拿到全部反馈：电角度 + 三相电流 + 母线电压 */
    bool (*read_feedback)(void *ctx, float *elec_angle_rad,
                          float *ia, float *ib, float *ic, float *vbus);

    /* 占空比 0.0 ~ 1.0，三相 */
    void (*set_duty_abc)(void *ctx, float da, float db, float dc);

    void (*enable_driver)(void *ctx, bool enable);
} foc_hw_t;
```

适配器 `App/Hardware/foc_hw_stm32.c` 是**唯一**同时知道 TIM8 + ADC1/2 + MT6835 的地方。

### 与本工程现有配置的衔接

`.ioc` 已经配好了这条链路，正是标准的低边采样结构：

```
TIM8 (中心对齐, ARR=4200, 20 kHz)
   └─ CH4 比较 (pulse=4100) ──触发──> ADC1 + ADC2 同步注入转换
                                          │
                                     ADC_IRQn (优先级 0)
                                          │
                    ┌─────────────────────┴─────────────────────┐
                    ▼                                           ▼
            读注入结果 (ia, ib)                        mt6835_read()  ← 实测 17.0 µs
                    └─────────────────────┬─────────────────────┘
                                          ▼
                          Clarke → Park → PI → 反 Park → SVPWM
                                          ▼
                                 更新 TIM8 CCR1/2/3
```

因此 **`mt6835_read()` 的调用点就是 `ADC_IRQHandler` / 注入转换完成回调**，
一个 20 kHz 周期一次，不需要额外的定时器。

---

## 8. 构建

统一走工程既有的脚本（不要把完整构建日志贴回来，脚本已经做了摘要）：

```bash
S=/e/405_FOC/.claude/skills/foc-build-flash/scripts/foc.sh
"$S" build Debug
"$S" build Release
"$S" flash Release
```

> ⚠️ 从 PowerShell 之类的非 msys2 终端调用 msys2 bash 时，必须用**登录 shell**
> 才有可用的 `/usr/bin`，同时**显式补上 Windows 侧工具链目录**，否则
> `cmake` / `arm-none-eabi-objcopy` 找不到：
>
> ```powershell
> & 'C:\msys64\usr\bin\bash.exe' -lc `
>   'export PATH=/d/DevEnv/GNU-tools-for-STM32/bin:/c/msys64/mingw64/bin:$PATH; "$S" build Debug'
> ```
>
> 直接用 `bash -c`（非登录）会导致 `/usr/bin` 不在 PATH 里，连 `ls` 都没有。

### `foc.sh debug` 在本机跑不起来（已知环境问题）

本机（装了 WSL/Hyper-V）的 Windows **TCP 保留端口段包含 4382~4481**，
OpenOCD 默认的 telnet 端口 4444 正好落在里面，于是启动即失败：

```
Error: couldn't bind telnet to socket on port 4444: No error
```

OpenOCD 会在打开 GDB 端口 3333 之前退出，表现为：

```
localhost:3333: Connection timed out.
```

查证命令：`netsh int ipv4 show excludedportrange protocol=tcp`

**解决办法（脚本只用于 GDB，根本不需要 telnet）**：在 `scripts/foc.sh`
的 `do_debug()` 两处 OpenOCD 调用里追加一个参数即可：

```bash
-c "telnet_port disabled"
```

本次验证是用同样的参数手工起 OpenOCD 完成的，没有改动 skill 脚本——
是否固化进脚本由你决定。
