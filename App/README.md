# 固件结构与使用

当前功能：M1 20 kHz 有感 FOC，Id_ref=0，Iq 命令范围 ±5 A、1 A/s 参考斜坡；MT6835 角度换相，实测母线补偿，居中 SVPWM。当前 PI 标称带宽 600 Hz。在电流环之上新增 **1 kHz 速度环与位置环（基础 P+I）**，输出仍是 Iq 参考；USB 20 kHz 改为 **`send 0..3` 四组分组日志**。每次启动先关断校零；已有 Flash 电角度记录时保持待机。首次/显式 cal 的对齐电压仍 0.6 V。本次实现与实测详见本地 [电流内环报告](../build/foc_analysis/REPORT.md)，旧 [tests/FOC_TEST.md](../tests/FOC_TEST.md) 属于历史电压模式。

## 模块

| 位置 | 职责 |
|---|---|
| `Control/app.c` | 初始化、串口命令、UART 2 kHz 和 USB 20 kHz 分组回传 |
| `Control/control.c` | 1 kHz 速度环/位置环（串级），目标接收、看门狗、多圈位置累积 |
| `FOC/foc.c` | 零偏、ABC/dq、双 PI/抗饱和、预测角度、SVPWM、校准状态、Iq 斜坡 |
| `Hardware/bsp/bsp_motor.c` | TIM8 功率输出、谷底更新/超时关断、校准 Flash |
| `Hardware/bsp/bsp_adc.c` | TIM8 采样触发、双 ADC 同步 DMA、电压换算、原始码值 |
| `Hardware/mt6835/mt6835.c` | 无 HAL 的 CRC 校验及角度解码 |
| `Hardware/mt6835/mt6835_port_stm32.c` | 固定板级 SPI3/PA0 接线、DMA 启停、原始角度 |
| `Hardware/bsp/bsp_can.c` | CAN1 非阻塞收发和接收队列 |
| `Hardware/bsp/bsp_uart.c` | UART DMA 发送、接收队列 |
| `Hardware/bsp/bsp_usb.*` | 原生 USB CDC、整帧异步发送队列、接收背压 |
| `Protocols/JustFloat/justfloat.h` | JustFloat 帧封装（UART 2 kHz 仍用） |

应用和协议不依赖 HAL；硬件驱动不调用应用。`Core/Src/main.c` 只调用 `app_init()`，主循环处理命令/校准保存后休眠；`stm32f4xx_it.c` 连接各驱动和应用回调。自写代码只使用生成文件的 USER CODE 块，其他代码保持 CubeMX 所有权。

USB 端口置 DTR 后，每个采样回调通过一次 `bsp_usb_write` 回传 **12 个小端 float32 + JustFloat 帧尾，共 52 字节**，1,040,000 B/s。下标 0 是打包时间戳/状态字，下标 1 是打包序号/组号，下标 2..11 由 `send X` 选定的日志组决定：

| 组 | 内容 |
|---|---|
| 0 | B/C/母线 ADC 原始码值、三路电压、编码器**未修正**角度、采样微秒、`b_offset` |
| 1 | Ib/Ic、电角度、`iq_ref`、PI 积分器 `integral_d/q`、`ud/uq`、零偏 |
| 2 | `ud/uq`、本周期生效的 CCR/占空比、采样窗口电压上限、`Vbus/√3` 线性上限 |
| 3 | Iq 参考与实测值、多圈 `pos_deg`、目标位置、编码器 `rpm`、目标转速、控制模式 |

通道表、位打包、时间/丢帧判据、分析配方见 [tools/bench/README.md](../tools/bench/README.md)。

UART 封装接口为 `uart_justfloat`，仍是 2 kHz、15 float。USB 需要 PC 持续接收，溢出会锁存；命令经 USB 或 UART 前台解析，支持实际 CR、LF 或 CRLF，不混入文本回显。

## 控制模式

`Iq <A>` / `rpm <v>` / `pos <deg>` 三条命令**命令即切模式**，无需额外 mode 命令。三者都通过同一个 PRECHARGE 联锁启动（2 ms 三低侧导通），`stop` 是唯一的下降路径。

- **Torque**：目标 Iq，与既有语义一致；`Iq 0` 待机时不启动，运行中保持零目标电流环。参考按 **1 A/s** 从上一个值斜坡到新目标（20 kHz 定步长），`stop` 清零并取消未完成的斜坡。
- **Speed**：1 kHz 速度环，目标 ±9400 RPM。复用 `foc.rpm` 的 20 kHz 编码器低通测速；外环计算在本周期 PWM 提交后进行，下一电流周期使用其 Iq 参考。
- **Position**：位置环 P 输出转速目标，再串速度环 PI，最终输出 Iq 参考。目标为**绝对多圈角度**（如 `pos 720.00` 表示两整圈），`zero` 把当前位置定义为 0°（需停机且静止）。当前增益 4 RPM/°，位置模式目标转速限幅 ±100 RPM。

参数在 `App/Control/control.c` 顶部常量块：`SPEED_KP=0.005`、`SPEED_KI=0.01`、`POSITION_KP=4`。位置环只有 P 项。外环输出限幅与 `Iq` 命令同为 ±5 A。

**主机看门狗**：Speed/Position 模式下若超过 **200 ms** 没有收到新目标，置 `FOC_UART` 并停机——外环握着计算出的参考值，PC 挂死必须能自停。主机回来后重新下发目标即可清除该故障。Torque 模式保持历史语义，没有该看门狗。

外环在 `foc_step` 的 `FOC_RUN` 分支内调度：`control_step()` 以 `motor_sample_us` 计时，每毫秒执行一次；同一周期内的其余 19 个采样点沿用上一个参考值。电流环的 PI、抗饱和、预测角度与 SVPWM 未改动。

## 采样链路

TIM8 中心对齐，168 MHz、PSC=0、ARR=4200、RCR=1，三相预装载在谷底统一生效；CH4 Toggle、CCR4=4100，OC4REF 作为 TRGO，上升沿每 50 us 触发双 ADC 规则同步转换。

ADC1 rank1=PC3/IN13（B 相），ADC2 rank1=PC2/IN12（C 相）；rank2 均为 PA6/IN6（母线）。ADC 时钟 21 MHz，采样时间分别 28/15 cycles，完整转换约 3.19 us。DMA2 Stream0/Channel0 循环读取公共 CDR，每次搬运两个 32-bit 字，第一个字完成后半传输中断。

1. ADC DMA 半传输中断启动 SPI DMA，与母线第二 rank 转换重叠。
2. SPI3（Mode 3、10.5 MHz）由 DMA1 Stream5/Channel0 发送 6 字节，Stream0/Channel0 接收；CPU 不等待 SPI。
3. SPI RX DMA 完成中断校验 CRC 和传感器状态，更新 `mt6835_angle_deg`，检查 ADC 两个 rank 已完成后发布三路电压，再调用 `app_sample()`。
4. 回调执行 FOC，在下一谷底前写入三相预装载；每 10 次回调提交一帧，完成 ADC/控制及 CCR 提交后立即启动 UART TX DMA1 Stream6/Channel4，避免 SysTick 启动相位漂移影响采样。

ADC 硬件转换及数据搬运不占 CPU 指令时间；电压换算、SPI 启动、CRC 解码仍需 CPU，不能称为零开销。ADC/SPI DMA IRQ 同为优先级 1；TIM8 更新、ADC 错误和 USART2 RX 为优先级 0。2 Mbps RX 每 5 us 一个字节，必须能够抢占 FOC 数学计算。

`adc_sample` 使用标称 VDDA=3.3 V，保留电流放大器偏置；母线倍率 41.2/2.2。前台读取完整三路快照须短暂关闭中断（正常数据在 SPI RX 完成中断发布，ADC 错误中断也会写入 NaN）。ADC overrun 或 DMA 错误停止采样，并置 NaN；编码器 CRC/状态/DMA 错误输出 NaN。`adc_errors`、`mt6835_errors` 只保留必要错误计数。

固定 CCR4=4100，检查本周期已生效和下一周期三相窗口，保证 B/C 同时低侧导通且 A 相不在孔径内切换。预留 500 ns 死区、3 us 建立及暂定 24 timer ticks 触发同步预算；电压矢量先按 Vbus/√3 限幅；低调制度保持居中 SVPWM，高调制度通过三相共模平移扩展低侧窗口，线电压不变。仅当相电压跨度仍放不下时缩放矢量，并将实际缩放反馈给 PI 反算抗饱和。默认时序下全角度可用幅值约 0.4759×Vbus，部分角度可更高；不能宣称全角度达到理论极限。上述建立/触发预算仍待示波器确认，不把数字窗口检查当成模拟精度证明。

启动时转子静止 200 ms 后采集 2048 对样本，独立估计 B/C 零偏与方差，零偏不存 Flash。默认标称换算 `(V-offset)/0.020` A，需要已知电流核验增益/极性。运行和校准均为 10 A 相峰值、运行 8600 RPM（电机额定上限）、8～36 V 母线保护。所有异常关闭功率并锁存，stop/clear 不自动恢复运行。

本台编码器正反向断功率滑行均测得约 0.52° 机械角二次谐波，FOC 使用 `angle += 0.52*cos(2*angle)` 修正后再测速、换相；复用现有查表，无电流低通。这是本台电机/磁铁安装的实测系数，更换编码器或磁铁位置后须重新核验。校准开始时重置累计角度基准，避免长时间运行后浮点精度不足导致行程误判。本轮数据和对照见 [台架报告](../build/bench_debug/20260921_1431_noise/REPORT.md)。

## 通信

USART2：PA2 TX / PA3 RX，**2000000 baud、8N1、无流控**。当前转换器 COM14 在 3.5 Mbps 下发送到 MCU 存在 RX 错误，2 Mbps 双向已验证。

当前应用以 2 kHz 发送 15 个小端 float32：`Id A、Iq A、B采样电压V、C采样电压V、采样序号、RPM、母线V、Iq参考A、电角度°、Ud V、Uq V、已生效占空比A/B/C、状态字`，随后 `00 00 80 7F`，共 64 字节，含 8N1 开销占 2 Mbps 的 64%。序号为 24 位整数，每帧增加 10；状态字转整数后，低 3 位为状态、随后 4 位为故障、随后 2 位为功率模式。

命令支持整数或最多两位小数，拒绝非法格式/非有限值/越界：

| 命令 | 范围 | 说明 |
|---|---|---|
| `Iq 0.20` | ±5 A | Torque 模式目标，1 A/s 参考斜坡 |
| `rpm -500` | ±9400 RPM | Speed 模式目标，**可正负** |
| `pos 720.00` | ±1e6° | Position 模式目标，绝对多圈机械角度 |
| `zero` | — | 把当前位置定义为 0°，需停机且静止 |
| `send 0`..`send 3` | — | 切换 20 kHz 日志组，纯日志开关 |
| `stop` | — | 立即关断，取消斜坡与外环 |
| `clear` | — | 仅清已消失故障，不自启 |
| `cal` | — | 仅静止待机重校准 |
| `hello` | — | 仅 UART 回 `#FOC 1.1 <状态字>`，不进 USB 二进制流 |

负 Iq 表示转矩方向，不等于立即负转速。`Iq 0` 运行中仍保持电流环，待机时不启动；`rpm 0`/`pos 0` 表示"保持在零转速/当前位置"，属于真实运行请求。乱码、超长或非法命令不会污染遥测：超长行整行丢弃，只递增 `app_command_rejected`（未定义命令除外，与历史行为一致）。Set/run 电压运行入口已移除。

`bsp_uart_write(data, size)` 复制后立即返回；双 256-byte TX 缓冲，满时整帧拒绝。`bsp_uart_read()` 从 127-byte 有效容量接收队列非阻塞取数据。`g_uart_stats` 记录拒绝、DMA 和 RX 错误。接收使用逐字节中断，不承诺满速持续双工；不得直接混用 HAL UART 收发。

CAN1：PB8 RX / PB9 TX，1 Mbps、标准/扩展/远程帧；`bsp_can_send()` 提交至硬件邮箱，不等待 ACK；`bsp_can_recv()` 从队列取帧。队列有效容量 15 帧，满时丢新帧并递增 `can_rx_lost`。应用不解释 CAN 命令；串口命令见上表，不混入文本回显。

## 构建与配置

工程根目录执行 `cmake --preset Release`、`cmake --build --preset Release`。`../download/flash.py flash Release` 自动选择匹配 F405/1MB 的 ST-Link 并校验烧录。Debug 用于调试，实时性用 Release 测量。

主机侧台架工具与数据集见 [tools/bench/README.md](../tools/bench/README.md)：`../download/bench.cmd list|discover|run|report|chain`。

`.ioc` 提供基础外设/引脚初始化；最终 ADC 规则同步、TRGO 和 DMA 配置由 BSP 在启动时覆盖。重新生成不会改动 App，但必须保留 USER CODE。DMA2 Stream0、DMA1 Stream0/5 已由驱动占用，不能分配给其他设备。
TIM5 由 `bsp_motor_init()` 配置为 1 MHz 自由运行计时，专用于 ADC DMA 入口的微秒时间戳；不能另作他用。

本次电流验证见本地 `../build/foc_analysis/REPORT.md`；此前电压模式历史结果见 `../tests/FOC_TEST.md`，采样链路与磁编见 `../tests/SAMPLING_TEST.md`，测试目录索引见 `../tests/README.md`，主机台架工具见 `../tools/bench/README.md`。旧测试入口、测速/EEPROM/多实例接口及通用 GPIO/SPI/时间包装已删除，HAL/CMSIS 原厂文件不做手工裁剪。

## 历史 Set 电压扩展验证（2026-09-20，不代表当前电流固件）

Debug/Release 与主机命令、角度、母线、采样窗口及 2 kHz 分频测试通过。最终 Release 上板正向空载分档末 2 s 转速：0.3 V≈135 RPM、1.2 V≈660 RPM、2.4 V≈1389 RPM、4.8 V≈2736 RPM。按电角度解包并除以 7 极对计算，仅供趋势参考。4.8 V 的 19 s 窗口收到 37997 帧，约 2 kHz；USB 窗口边界不用于断言零丢帧。最终 ADC/磁编/UART 错误及命令拒绝均为 0，状态 IDLE/故障 0，六路栅极 GPIO 低，Flash 校准记录保留。

最终 motor_work_max=3075 cycles（18.30 us），区间从 ADC DMA IRQ 入口至控制及遥测完成，包含 SPI 等待，并非纯 CPU 占用。TIM8 PSC=0、ARR=4200、RCR=1。最小周期统计回读为 0，尚不能用本次极值统计验收全部中断周期。首次版本曾触发时序保护，减少重复除法及限幅库调用后完成上述四档测试，未放宽保护。

原始日志在本地 build/foc/set_*.bin、CSV、JSON。未测反向高速、相电流准确值、MOS/ADC 模拟波形；电源限流与实际电流未由软件读回。旧 `tests/FOC_TEST.md` 和 `tests/legacy/capture_foc.py` 为旧 48 字节协议记录/工具，不适用于新的 24 字节协议。


## 本次电流模式验证（2026-09-20）

主机回归、Debug/Release/独立 Capture 构建及 diff 检查通过。上板 ±0.1/±0.2 A 各约 1 s，最后 0.4 s 反馈 Iq 均值为 +0.09672/-0.10261/+0.19602/-0.20147 A，瞬时标准差 0.023～0.038 A；部分样本超出 ±0.05 A 误差带。相电流仍为标称换算，增益/极性和示波器窗口验收待完成，没有测试 ±0.5 A 或验证持续旋转性能。

四档运行链路最大 3411 cycles=20.304 us，包含 SPI 等待与遥测，非纯 CPU 占用；600 ticks 写 CCR 截止保护保持原值。最终普通 Release 重烧后被动 10 s 收到 19993 帧、相邻序号全差 10、故障/ADC/磁编/UART 错误为 0，GPIO 六路低、校准 Flash 记录保留。当前构建最后停机逻辑修正只做了主机与停机上板回归，四档加压为修正前相同控制算法结果。

独立 `-DFOC_CAPTURE=ON` 构建支持 capture/dump，2048 条 32-byte 记录、满后关断，停止后导出；普通 Release 不含此 64 KiB 缓冲。新工具是本地 `build/foc_analysis/current_serial.py`（默认 stop 后被动接收），旧测试脚本属于历史 API/协议，不能混用。

## 电流采样专项核验（2026-09-20，后续于上述历史记录）

本轮报告及连续原始记录在 `../build/current_audit/REPORT.md`。正常协议仍为 2 kHz、14 float；只调整 TX 启动相位，保持 20 kHz 控制、300 Hz PI、±0.8 A 指令和保护阈值。CCR4 对比后仍选 4100，窗口限幅同时约束前后沿。

独立 Capture 构建改为 FOC2：8 字节头（FOC2 和 uint32 条数），每条 36 字节 `<I8H4f>`：序号、B/C/母线 raw、ADC-read 入口 CNT、已生效三相 CCR、flags、机械角度、采样电角度、Id、Iq。flags 位 0～2 为状态、3～6 为故障、7～8 为采样功率模式、9 为窗口有效；CNT 不是采样保持结束时间。2048 条共 72 KiB，普通 Release 不包含。

`quiet 1/0` 暂停/恢复正常遥测；`capture` 连续抓取并在满后关断；静止待机可用 `capture low` / `capture zero` 比较三低侧导通/零目标 PWM。`dump` 关断后导出，`stop` 取消采集并恢复遥测。旧 FOC1 工具不适用；新工具在本地 `build/current_audit`。

静止串口相关周期分量有改善，高速 Id 波动和绝对电流精度尚未验收；没有提高 PI 带宽或扩大电流测试范围。DPS-150 USB 通信已恢复，仅读取设置和测量值，实际设定 12.65 V/0.5 A，未修改。

## 24 V 母线适配（后续版本）

母线运行及 clear 条件统一为 8～28 V。输出矢量取消固定 4.8 V 上限，仍受 B/C 低侧有效采样窗口约束：默认 CCR4=4100 时 `|Udq| ≤ 0.374454×Vbus`，24 V 时约 8.99 V。±0.8 A 指令、3000 RPM 超速、运行相电流峰值 2 A、500 ns 死区及 600 ticks 提交截止保持。升压提供高速反电动势余量，不等于相同 Iq 下转矩翻倍。24 V 本轮测试记录另见本地 `build/bus24/REPORT.md`，历史12.6 V测试不能作为24 V验收结果。

## 8～36 V 自测版本（后续于24 V版本）

源代码母线允许范围改为8～36 V，clear与运行使用同一边界。Iq命令仍为 `Iq 0.60\r\n`，单位A，范围±0.8 A、最多两位小数、1 A/s斜坡；`Iq 0`保持零目标电流环，`stop`关闭功率，`clear`清故障不自启。20 kHz控制和2 kHz原14-float遥测不变。

首轮3000 RPM阈值改为电机文档的8600 RPM超速跳闸阈值，同时改进预测旋转余弦近似；采样窗口、运行2 A相电流峰值和时序故障保护保留。电压矢量仍按采样窗口限幅，36 V时约13.48 V，不再固定4.8 V。36 V及高速尚未进行实机验收，本次只构建、未烧录；电机本体文档额定24 V，驱动输入范围并不等同电机任意工况额定范围。


## 本轮窗口修正与电流响应迭代（2026-09-21）

当前 PI 标称带宽600 Hz（Kp=0.18849556 V/A，KiTs=0.02261947 V/A），采样/控制仍20 kHz，普通遥测仍14 float、2 kHz。运行参考不再有1 A/s斜坡；首次上电对齐仍为0.6 V并保留校准斜坡。ADC孔径28 cycles、CCR4=4100和500 ns死区未缩短。

`motor_write_min` 为写CCR入口最小CNT，要求至少600 ticks；`motor_timing_fault`低字节分别为1提交、2谷底更新、3采样周期，高位记录当时CNT或间隔。它们用于定位超时，不改变截止条件。

独立Capture构建现在为FOC3：8字节头（FOC3、uint32条数），每条48字节`<I8H7f>`，前36字节与FOC2相同，末尾增加实际Iq参考、Ud_next、Uq_next。2048条占96 KiB，仅用于调试；普通Release不含该缓冲。ADC-read入口CNT现在在SPI完成时记录，不能与旧版的ADC DMA入口CNT直接比较。

本轮本地报告、主机测试、原始2k/20k记录及供电日志在`../build/current_iteration/`。已完成短时±0.8 A跨零，300/500/600 Hz对比；约4200 RPM的Id标准差由0.846 A降至0.605 A，但绝对电流精度及高速波动仍未验收。主动反转长测两次因电源输入约32 V跌至约4 V而中断，电源保护码5（欠压）；不能记为一分钟耐久通过。详细后续测试以本轮报告为准，历史记录保留。

## 串口与 Debug 修复（2026-09-21，当前版本）

3.5/3 Mbps 命令接收复测出现 RX 错误，现用 2 Mbps、四字段 5 kHz，ADC/FOC 保持 20 kHz。VOFA 使用文本命令 `Iq 0.40` 并附加实际 CRLF；`stop` 关断。

Debug 原先以 -O0 编译实时路径，板上出现 FOC_TIMING 故障，随后 Iq 命令被拒绝而遥测继续。现 Debug/Release 均对已有七个实时源文件使用 -O3，Debug 保留调试信息（优化会影响单步和局部变量可见性），不放宽 600 ticks 截止。Debug 实测正反转及运行中连续 100 条命令通过，UART/命令拒绝/时序故障均为 0，最小提交 CNT 为 879。Debug/Release 构建通过。上述历史章节保留当时参数和结果，不作为当前协议说明。
