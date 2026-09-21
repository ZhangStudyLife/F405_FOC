> 历史电压模式记录，命令和遥测不适用于当前固件；当前使用说明见 ../App/README.md。

# M1 电压模式有感 FOC：实现和实测

2026-09-20，F405 / 168 MHz，ST-Link `8600A1002031363534313541`，COM14 / 2000000 / 8N1。
用户确认 J9/DCBUS=12.6 V，5010-KV360 空载、转轴可自由转动。没有进行电流或速度闭环。

## 操作

首次无记录时自动校准；有效记录存在时上电保持待机。校准会转动电机。
原定 0.3 V 校准失败，经用户授权一次 0.6 V 测试成功，用户随后确认默认改为 **0.6 V**。
固定母线计算值为 12.6 V，ADC 母线值仍发布到 `adc_sample`，不参与 PWM。

命令使用 ASCII 换行结尾（LF，接受 CRLF），不要混入 JustFloat 二进制发送：

| 命令 | 行为 |
|---|---|
| `run 0.3` | 校准有效且待机时启动，Ud=0，Uq 按 0.3 V/s 缓升；限制 ±0.6 V |
| `run -0.3` | 反方向；运行中禁止直接换向，先 stop |
| `stop` / `run 0` | 立即关断六路栅极；故障锁存不清除 |
| `cal` | 仅待机可重新校准，成功才覆盖保存记录 |
| `clear` | 数据已恢复且新鲜时解除故障到待机，不自动启动 |

拒绝非法命令、NaN/Inf、越界电压、过长行；`app_command_rejected` 计数。
不发送文本应答，通过状态及故障字段判断结果。UART RX 错误/丢字节在运行中触发关断，损坏行丢弃到下一换行。
串口完全拔出没有自动超时停机功能，操作程序必须在关闭连接前确认 stop；没有电流反馈保护。

`python tests/capture_foc.py --seconds 5 --uq 0.3 --out build/foc/trial`
执行限时测试，收到待机/故障且占空比全零才关闭串口；没有 `--uq` 时仅被动记录。
输出 BIN、CSV、JSON。需要 pyserial。状态不满足待机时测试不会启动；运行失败返回非零。

## JustFloat 帧与量纲

1 kHz，每 20 次 20 kHz 回调一次。小端 11 个 float + `00 00 80 7F`，每帧 48 bytes：

`毫秒时间戳, dA, dB, dC, 机械角度°, B_ADC_V, C_ADC_V, RPM, Uq_V, state, fault`

- dA/B/C 为采样周期实际生效的高侧 CCR/4200，0～1，不是下一周期预装载，也不是示波器测量的栅极占空比。
- 停机/自举预充三路高侧占空比为零；预充时三路低侧导通 2 ms。
- B=PC3/ADC1_IN13/M1_SO1；C=PC2/ADC2_IN12/M1_SO2。`raw*3.3/4095`，保留偏置，没有电流换算。
- 机械角为 0～360°；RPM 由 20 kHz 解包角差计算，系数 0.01 的一阶滤波，仅作观察，不用于闭环。
- state：0 待机、1 预充、2 校准、3 保存、4 运行、5 故障。
- fault：0 正常、1 磁编、2 ADC、3 周期/截止时间、4 采样窗口、5 校准、6 Flash、7 UART。
- 保存前发布 state=3；Flash 擦写暂停采样。单 Bank Flash 擦除也可能阻塞 SysTick，跨保存间隔不能用 MCU 毫秒戳测真实墙钟耗时。
- 校准期间 Uq=0，施加的是 Ud，最终默认 0.6 V；校准曲线不能按运行 Uq 解读。

## 时序及保存边界

TIM8 PSC=0、ARR=4200、中心对齐、RCR=1；UG 在 CNT=0 装载，三相预装载只在谷底生效。
启动强制 CH4 OCREF 为低再切 Toggle，CCR4=4100，上数触发。
ADC1/2 规则同步 DMA，rank1 同时采 B/C（28 cycles），rank2 双 ADC 均采 IN6（15 cycles），21 MHz，总转换 3.19 us。
ADC DMA 完成后启动磁编 SPI DMA，完成时执行 FOC，下一谷底前至少留 600 timer ticks 的写入余量。

B/C 采样保持窗口为 224 timer ticks；检查前后留出 84 ticks 死区及 504 ticks（3 us）模拟建立余量。
±0.6 V 下数学占空比约 45.9%～54.1%，本次实际 0.3 V 约 47.93%～52.07%。
这只是软件边界与空载测试，不等于已用示波器验证所有模拟建立时间。

TIM8 更新 IRQ 检查本周期控制结果是否到齐，遗漏则关断；关断锁存防止高优先级故障抢占后旧的 PWM 写入重新启用栅极。
ADC 错误先关栅极再停采样；前台恢复采样供观测，电机保持故障。
HardFault、MemManage、BusFault、UsageFault、NMI、Error_Handler 均执行关断。

Flash sector11（0x080E0000，128 KiB）保留，链接空间 896 KiB。
记录为版本、7 极对、零点 rad、方向 ±1、FNV-1a 校验、最后写入的完成标记；无效/撕裂记录下次重新校准。
写入前关功率、停止 ADC 和 SPI；主循环擦写、锁回 Flash、回读后恢复采样。
更换磁编安装或相线必须重新 cal。全片擦除丢失校准。

## 本次验证

- Debug / Release 构建通过，无新增编译警告；diff 空白检查通过。
- 主机测试：`test_foc.c` 验证六扇区/线电压、跨零、正反校准、堵转失败、电压缓升和窗口；`test_foc_commands.c` 验证命令/关断/通信故障及记录所有单 bit 损坏。原 CRC、JustFloat 测试通过。
- 关闭功率读回 TIM8、GPIO，确认预装载、RCR=1、PSC=0、ARR=4200、CCR4=4100、BDTR=0x8054，六路 GPIO 低。
- 0.3 V 校准：机械角总范围约 4.8°，未跟随预期 51.4°，锁存校准故障，无记录写入。
- 0.6 V 校准：初始稳定约 156.66°，正向结束约 207.75°，返回稳定约 156.78°；正方向，保存零点约 **0.305118 rad**。回位/稳定检查通过。
- 存储：禁止功率输出的临时固件测试写入/读取，另一次仅加载固件重烧确认 sector11 保留；测试记录随后擦除。真实校准记录在后续多次重烧及复位后也保持，启动待机。
- 0.15 V：与初始转角有关，不能保证持续启动；本次最终分档记录主要小幅位移。未自动提高运行电压。
- 0.3 V：已持续旋转；最终 5 秒运行后收到停止确认，5250 帧（含约 250 ms 停机确认），帧间隔全 1 ms，无内部错帧。3.0～4.9 s 稳态平均约 **135.6 RPM**，范围 **123.9～149.9 RPM**；不是恒速控制。
- 最终版停止后 30 秒：**30000 帧**，全部 1 ms，内部字节间断=0。
- 最终板上快照：ADC、磁编、UART 错误/丢失、命令拒绝均为 0；电机待机，六路栅极 GPIO 低。
- 临时固件在运行时翻转一个磁编 CRC bit，观测故障1、磁编错误+1及六路关断；故障注入代码已移除。未实际拔掉磁编。
- SWD 阻断 SPI DMA IRQ 后观测故障3及关断。CubeProgrammer 对 NVIC 写操作报下载校验失败，故只将关断状态作为观察结果，不宣称精确故障注入延迟测量。
- 原 UART RX 低优先级在带 FOC 运算时发生溢出，已改为优先级0抢占 ADC/SPI 运算（优先级1）；重测运行/停止及最终日志计数为0。

最终周期统计 **8310～8494 CPU cycles（49.464～50.560 us，中断入口间隔；硬件触发周期仍为50 us）**；ADC DMA 入口到 FOC/遥测结束最大 **3159 cycles（18.804 us）**，加 ADC 转换约21.99 us。该值是整条链路延迟，尚未单独测最终 CPU 忙碌时间。

最终时序统计与原始数据在 `build/foc/final_registers.json`、`final_run_030.*`、`final_idle_30s.*`。
`motor_work_max` 测的是 ADC DMA IRQ 开始到磁编/FOC/遥测处理结束的墙钟延迟，包含 SPI DMA 传输等待，不是 CPU 忙碌时间，也不包含前面的 ADC 转换及完整异常进出开销。
不得与以前单段 ADC ISR 的 CPU 测量直接比较。

## 待验证

物理断电重启（已测 MCU 复位，不能代替断电）、MOS/死区/相电流示波器波形、带载及全电压范围、VDDA 精度和采样偏置标定。
低侧放大器输出只有数 mV 变化时量化和偏置误差明显；1 kHz 日志不能还原 20 kHz 开关纹波，不能凭此次空载波形宣称电流精度已达标。

原采样固件的历史实测保留在 `tests/README.md`，不作为本次 FOC 结论。

## 主机测试命令

```powershell
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/FOC tests/test_foc.c App/FOC/foc.c -lm -o build/test_foc.exe
./build/test_foc.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Control -I App/FOC -I App/Hardware/bsp -I App/Hardware/mt6835 -I App/Protocols/JustFloat tests/test_foc_commands.c App/Control/app.c App/FOC/foc.c -lm -o build/test_foc_commands.exe
./build/test_foc_commands.exe
```

本机曲线：[完整运行](../build/foc/final_run_030.png)、[马鞍形局部](../build/foc/saddle_detail.png)、[CSV](../build/foc/final_run_030.csv)。build 下原始数据不进入 Git。
