# ADC 上板采集验收

日期：2026-09-14。设备：STM32F405 + ST-Link V2，OpenOCD 0.12.0。
本轮目标：六路功率输出关闭时，独立验证 20 kHz 双 ADC 注入采样。

## 实现与范围

- `bsp_adc` 固定 ADC1/ADC2、各两档，去掉任意句柄、可选从机和 1～4 档抽象。
- 先检查双 ADC 模式、通道顺序、分辨率、对齐、扫描、ADC 时钟、采样时间及触发源。
- 只开 ADC1 JEOC 中断；读取前检查两个 ADC 都完成，读取四个 JDR 后只清注入标志。
- `adc_test` 负责计数、周期统计和 RAM 录波，不依赖编码器、PI、SVPWM 或电流换算。
- 主循环每 1 ms 用 `HAL_UART_Transmit_DMA(&huart2, ...)` 发送一帧 16 字节 ADC 原始调试数据；DMA 忙时跳过，不阻塞采样 ISR，USART2 IRQ 负责 DMA 发送完成收尾。
- 配置错误时不启动定时器；主从完成状态不一致时计错并停止采集，避免使用旧值。
- 旧 FOC 装配代码保留但不参与构建；撤下旧文档中不可复核的实测表和带功率校准步骤。
- 删除链接脚本里保留未调用调试函数的 `.foc_debug` KEEP；直接通过 SWD 读数据。

## 触发与功率引脚

TIM8 中心对齐，168 MHz，PSC=0，ARR=4200，CH4 Toggle，CCR4=4100，
ADC1 选择 TIM8_CC4 上升沿。ADC2 从主 ADC 同步启动。
ADC 时钟 21 MHz，rank1 为 28 周期采样，rank2 为 15 周期采样。

F405 注入组没有 TIM8_TRGO 选项。本轮实测 CH4 触发受 MOE 门控：
关闭 MOE 后两次计数均为 616515，恢复后达到 620539。
因此测试用 MOE=1、CCER=0x1000，只使能 CH4。
PA7、PB0、PB1、PC6、PC7、PC8 从定时器复用切换为推挽 GPIO 低电平。
脚本检查 MODER、OTYPER、ODR、IDR；这不是示波器对栅极电压的测量。

当前 CCR4 仅用于验证触发与采集。没有带 PWM 测过低侧有效采样窗口，
不能将这个测试初始化直接用于电机运行。

UART2 调试帧格式为小端序：`A5 5A`、uint16 序号、4 个 uint16 原始值（PC3、PC2、PA4、PA6）、uint32 ADC 帧计数。
串口参数为 460800、8N1、无流控；PA2 接 USB-UART RX，必须共地。

## 实测结果

Debug / Release 均构建无警告，分别烧录并通过 OpenOCD verify。
每份连续记录为 1024 帧（51.2 ms）；另用约 2 秒主机计数差验证持续运行。

- 连续帧间隔：8399～8402 CPU 周期；按标称 168 MHz 换算为约 20,000 Hz。
- 主从完成异常：0。启动后的周期极值检查也通过，没有出现整拍丢失。
- 追加连续运行检查：10.001653 秒新增 200035 帧，主机估计 20000.19 Hz；
  累计 2186345 帧时异常仍为 0，周期极值为 8385～8400。
- 六路功率引脚均为 GPIO 推挽低电平，CH1～CH3 和互补输出均未使能。
- Release ISR 测量区间最大 240 周期，约 1.43 µs，包含读数和录波；
  不包括异常进出栈、完整函数序言/尾声，不能当作未来整套 FOC 电流环耗时。
- 启动后的首个周期存在约 15～17 周期的差异；上述固定间隔来自稳定运行窗口。

Release 最终记录，单位为 ADC 原始码：

| 引脚 | 均值 | 标准差 | 最小 | 最大 |
|---|---:|---:|---:|---:|
| PC3 | 2062.645 | 1.490 | 2057 | 2068 |
| PC2 | 2088.180 | 1.590 | 2083 | 2096 |
| PA4 | 1145.385 | 1.832 | 1136 | 1152 |
| PA6 | 796.554 | 1.627 | 789 | 802 |

这里的电流通道均值只是本次无功率输出状态的偏置观测，不是永久零偏标定。
未验证已知电压输入的 ADC 精度、电流增益/极性、母线分压或温度公式；
晶振绝对误差未用外部仪器测量。主从 JEOC 与配置检查验证了数字采集完整性，
没有通过外部激励测量两路模拟孔径之间的偏差。

## UART2 DMA 验证

Debug 固件通过 SWD 先停发周期测试帧，确认 UART 空闲后调用一次
`HAL_UART_Transmit_DMA()`，返回 `HAL_OK`；恢复运行 100 ms 后观察到
`huart2.gState=READY`、DMA `NDTR=0`、`CR=0`。这验证了 DMA1 Stream6
搬运和 USART2 发送完成中断收尾。当前电脑未检测到可用 USB-UART 适配器，
因此 PA2 到上位机的实际字节抓取仍需接入 3.3 V 电平串口适配器后进行。

## 故障路径验证（Debug，上板）

1. 在 `bsp_adc_start` 入口硬件断点处，将 ADC2_JSQR 清零再继续：
   `started=0`、`samples=0`，TIM8 CEN=0，配置不匹配被拒绝。
2. 复位，在第一次 `adc_test_isr` 入口断点处清 ADC2_SR 再继续：
   `incomplete_pairs=1`、`samples=0`，TIM8 CEN=0，未发布不完整帧。
3. 每项后复位恢复原配置。最终重新烧录正常 Release 固件，保持连续采集。

## 复测

从项目根目录执行（ARM GCC、CMake、Ninja 已在 PATH）：

```powershell
cmake --preset Release
cmake --build --preset Release
& 'D:/DevEnv/openocd-v0.12.0-i686-w64-mingw32/bin/openocd.exe' -s 'D:/DevEnv/openocd-v0.12.0-i686-w64-mingw32/share/openocd/scripts' -f interface/stlink.cfg -f target/stm32f4x.cfg -c 'gdb_port disabled' -c 'telnet_port disabled' -c 'tcl_port disabled' -c 'program build/Release/405_FOC.elf verify reset exit'
python tools/adc_capture.py --elf build/Release/405_FOC.elf
```

先关闭占用 ST-Link 的调试器。脚本复用 `scope_lib.py` 的本机 OpenOCD 路径。
成功返回 0，验收失败返回非 0；保存 CSV 原始记录和 JSON 寄存器/统计值。
冻结的是 RAM 录波，不是 CPU，读完会恢复录波。
若期间用调试器停过核，请复位后再检查全程周期极值。

SWD 可观测：`g_adc_test`（启动结果、帧数、四路最新原始值、周期极值、
ISR 测量区间最大耗时、主从不完整次数）；`s_adc_scope`（六通道环形缓冲）。
`started` 表示初始化成功，不表示故障后仍在运行；同时检查帧数增长和异常计数。

本地证据保存在 `validation/adc-debug-final/`、`validation/adc-release-final/`，
以及 `validation/adc-negative-tests.json`、`validation/adc-moe-gating.json`、
`validation/adc-release-soak.json`。
`validation` 为原项目忽略目录，不随 Git 提交；本文件保留结果及复测步骤。

最终 Release ELF SHA-256：

```
014a6a2fcb7ea65c583484b6b1eaca055ed6d69320ff1296cb012eb37f0bb853
```
