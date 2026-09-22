# FOC 电压模式历史记录（2026-09-20）

> **这是历史记录。** 本文件描述的是 2026-09-20 的 **M1 电压模式**固件：命令是 `run <V>` / `Set <V>`，
> 遥测是 48 字节 / 11 float，已全部被当前**电流模式**取代。当前接口、参数与用法见
> [App/README.md](../App/README.md)（`Iq <A>` / `stop` / `cal` / `clear`，UART 2 kHz 64 字节与 USB 20 kHz 36 字节）。
> 当前电流内环的实现与实测见本地 `../build/foc_analysis/REPORT.md`、`../build/current_audit/REPORT.md`、
> `../build/current_iteration/`、`../build/bench_debug/20260921_1431_noise/REPORT.md`（这些目录不入版本库）。
> 采样链路与磁编记录见 [SAMPLING_TEST.md](SAMPLING_TEST.md)，两种回传见 [UART_TEST.md](UART_TEST.md) 与 [USB_TEST.md](USB_TEST.md)。

## 条件

2026-09-20，STM32F405 / 168 MHz，ST-Link `8600A1002031363534313541`，COM14 / 2000000 / 8N1。
用户确认 J9/DCBUS=12.6 V，5010-KV360 空载、转轴可自由转动。**没有做电流或速度闭环。**
固件使用固定母线计算值 12.6 V，ADC 母线值仍发布到 `adc_sample`，但不参与 PWM。

## 操作

首次无记录时自动校准；有效记录存在时上电保持待机。校准会转动电机。
原定 0.3 V 校准失败，经用户授权一次 0.6 V 测试成功，用户随后确认默认改为 **0.6 V**。

命令为 ASCII 换行结尾（LF，接受 CRLF），不要混入 JustFloat 二进制：

| 命令 | 行为 |
|---|---|
| `run 0.3` | 校准有效且待机时启动，Ud=0，Uq 按 0.3 V/s 缓升；限制 ±0.6 V |
| `run -0.3` | 反方向；运行中禁止直接换向，先 stop |
| `stop` / `run 0` | 立即关断六路栅极；故障锁存不清除 |
| `cal` | 仅待机可重新校准，成功才覆盖保存记录 |
| `clear` | 数据已恢复且新鲜时解除故障到待机，不自动启动 |

拒绝非法命令、NaN/Inf、越界电压和过长行；拒绝计入 `app_command_rejected`。不发送文本应答，通过状态与故障字段
判断结果。UART RX 错误/丢字节在运行中触发关断，损坏行丢弃到下一换行。
**串口完全拔出没有自动超时停机功能**，操作程序必须在关闭连接前确认 stop；当时也没有电流反馈保护。

限时试验工具（现在位于 [legacy/capture_foc.py](legacy/capture_foc.py)，只适用于旧协议）：

```sh
python tests/legacy/capture_foc.py --seconds 5 --uq 0.3 --out build/foc/trial
```

无 `--uq` 时仅被动记录；有 `--uq` 时要求进入前状态为待机/无故障，结束时必须收到关断遥测，否则非零退出。
输出 BIN、CSV、JSON，需要 pyserial。

## 历史帧与量纲（48 字节）

1 kHz，每 20 次 20 kHz 回调一次。小端 11 个 float + `00 00 80 7F`：

`毫秒时间戳, dA, dB, dC, 机械角度°, B_ADC_V, C_ADC_V, RPM, Uq_V, state, fault`

- dA/B/C 是采样周期实际生效的高侧 CCR/4200（0～1），不是下一周期预装载，也不是示波器测到的栅极占空比；
  停机/自举预充时三路高侧占空比为零，预充时三路低侧导通 2 ms。
- B=PC3/ADC1_IN13/M1_SO1，C=PC2/ADC2_IN12/M1_SO2；`raw × 3.3/4095`，保留偏置，没有电流换算。
- 机械角 0～360°；RPM 由 20 kHz 解包角差计算，0.01 系数一阶滤波，仅作观察，不用于闭环。
- `state`：0 待机、1 预充、2 校准、3 保存、4 运行、5 故障。
- `fault`：0 正常、1 磁编、2 ADC、3 周期/截止时间、4 采样窗口、5 校准、6 Flash、7 UART。
- 保存前发布 state=3；Flash 擦写暂停采样。单 Bank Flash 擦除也可能阻塞 SysTick，
  跨保存间隔不能用 MCU 毫秒戳测真实墙钟耗时。
- 校准期间 Uq=0、施加的是 Ud，最终默认 0.6 V；校准曲线不能按运行 Uq 解读。

## 时序与保护

TIM8 PSC=0、ARR=4200、中心对齐、RCR=1；UG 在 CNT=0 装载，三相预装载只在谷底生效。
启动强制 CH4 OCREF 为低再切 Toggle，CCR4=4100，上数触发。
ADC1/2 规则同步 DMA：rank1 同时采 B/C（28 cycles），rank2 双 ADC 均采 IN6（15 cycles），21 MHz
（序列耗时与链路细节见 [SAMPLING_TEST.md](SAMPLING_TEST.md)）。ADC DMA 完成后启动磁编 SPI DMA，
完成时执行 FOC，下一谷底前至少留 600 timer ticks 写入余量。

B/C 采样保持窗口为 224 timer ticks；前后留 84 ticks 死区及 504 ticks（3 µs）模拟建立余量。
±0.6 V 下数学占空比约 45.9%～54.1%，本次 0.3 V 实测约 47.93%～52.07%。这只是软件边界与空载测试，
不等于已用示波器验证所有模拟建立时间。

TIM8 更新 IRQ 检查本周期控制结果是否到齐，遗漏则关断；关断锁存防止高优先级故障抢占后旧的 PWM 写入
重新启用栅极。ADC 错误先关栅极再停采样，前台恢复采样供观测、电机保持故障。
HardFault、MemManage、BusFault、UsageFault、NMI、Error_Handler 均执行关断。

## Flash 校准记录

Flash sector 11（0x080E0000，128 KiB）保留，链接空间 896 KiB。记录为版本、7 极对、零点 rad、方向 ±1、
FNV-1a 校验和最后写入的完成标记；无效/撕裂记录下次重新校准。写入前关功率、停止 ADC 和 SPI；
主循环擦写、锁回 Flash、回读后恢复采样。更换磁编安装或相线必须重新 cal；全片擦除丢失校准。
（当前固件仍使用 `App/Hardware/bsp/bsp_motor_record.h` 中的同一记录结构。）

## 本次验证（2026-09-20）

- Debug / Release 构建通过，无新增编译警告；diff 空白检查通过。
- 关闭功率读回 TIM8 与 GPIO，确认预装载、RCR=1、PSC=0、ARR=4200、CCR4=4100、BDTR=0x8054，六路 GPIO 低。
- 0.3 V 校准：机械角总范围约 4.8°（`build/foc/calibration_03.csv` 中 state 2 期间 164.284°～169.086° 即 4.80°），
  未跟随预期 51.4°（=360°/7），随后锁存校准故障（state 5 / fault 5，4483 帧），无记录写入。
- 0.6 V 校准（`build/foc/calibration_06.csv`，6927 帧）：初始稳定约 156.66°（t≈0.5 s / 1.5 s 为 156.687° / 156.662°），
  正向峰值约 207.86°，返回稳定约 156.78°；方向为正，保存零点约 **0.305118 rad**（`build/foc/final_registers.json`）。
  抓包窗口首/末角度 139.70° / 165.24° 是对齐前与停机后的静置角，不是校准极值。
- 存储：禁止功率输出的临时固件测试写入/读取，另一次仅加载固件重烧确认 sector11 保留；测试记录随后擦除。
  真实校准记录在后续多次重烧及复位后保持，启动待机。
- 0.15 V：与初始转角有关，不能保证持续启动；最终分档记录主要小幅位移，未自动提高运行电压。
- 0.3 V：已持续旋转；最终 5 秒运行后收到停止确认，5250 帧（含约 250 ms 停机确认），帧间隔全 1 ms，
  无内部错帧。抓包相对 3.0～4.9 s（时间戳 3292～5192 ms）的 1901 个 RUN 样本统计：平均 **135.64 RPM**、
  最小 123.86、最大 149.95（由 `build/foc/final_run_030.csv` 直接统计）；不是恒速控制。
- 最终版停止后 30 秒：**30000 帧**，全部 1 ms，内部字节间断 0。
- 最终板上快照：ADC、磁编、UART 错误/丢失与命令拒绝均为 0；电机待机，六路栅极 GPIO 低。
- 临时固件在运行时翻转一个磁编 CRC 位，观测到故障 1、磁编错误 +1 及六路关断；注入代码已移除，
  未实际拔掉磁编。SWD 阻断 SPI DMA IRQ 后观测到故障 3 及关断（细节见 [SAMPLING_TEST.md](SAMPLING_TEST.md)）。
- 原 UART RX 低优先级在带 FOC 运算时发生溢出，已改为优先级 0 抢占 ADC/SPI 运算（优先级 1）；
  重测运行/停止及最终日志计数为 0。

最终周期统计 8310～8494 CPU cycles（49.464～50.560 µs，中断入口间隔；硬件触发周期仍为 50 µs），
ADC DMA 入口到 FOC/遥测结束最大 3159 cycles（18.804 µs），加 ADC 转换约 21.99 µs。
该值是整条链路延迟，不是最终 CPU 忙碌时间；口径说明见 [SAMPLING_TEST.md](SAMPLING_TEST.md)。

本机原始数据：`build/foc/final_registers.json`、`final_run_030.*`、`final_idle_30s.*`、`calibration_03.*`、
`calibration_06.*`、`run_0*_*.json`、`set_*.json`；曲线为 `build/foc/final_run_030.png`、`saddle_detail.png`。
`build/` 不入版本库。

## 历史主机测试

当时的两个测试覆盖六扇区/线电压、跨零、正反校准、堵转失败、电压缓升与采样窗口，以及命令/关断/通信故障
和校准记录的全部单比特损坏。它们针对已被删除的电压模式 API，**不能**对当前固件编译运行，
现存放于 `tests/legacy/`（失效原因见 [README.md](README.md) 的"历史资产"一节）：

```sh
# 仅作历史参考：对当前头文件编译会失败
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/FOC tests/legacy/test_foc.c App/FOC/foc.c -lm -o build/test_foc.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Control -I App/FOC -I App/Hardware/bsp -I App/Hardware/mt6835 -I App/Protocols/JustFloat tests/legacy/test_foc_commands.c App/Control/app.c App/FOC/foc.c -lm -o build/test_foc_commands.exe
```

第一条在 `foc_step` / `foc_modulate` / `foc_run` 上报错；第二条要带上上面全部 include 目录，否则会先缺
`justfloat.h`，走不到同样的 API 报错。当前可用的主机测试（含校准状态机回归 `test_foc_recalibration.c`）见
[README.md](README.md)。

## 未验证

物理断电重启（只测过 MCU 复位，不能代替断电）、MOS/死区/相电流示波器波形、带载及全电压范围、
VDDA 精度和采样偏置标定。低侧放大器输出只有数 mV 变化时量化和偏置误差明显；1 kHz 日志不能还原
20 kHz 开关纹波，不能凭此次空载波形宣称电流精度已达标。
