# tests 目录索引

本目录保存**主机端回归测试**、**PC 侧验证脚本**和**实机测试记录**。固件接口、参数与当前配置见
[App/README.md](../App/README.md)；`build/` 下的原始数据、JSON 和报告只在本机保留，不进入版本库。

## 代码

| 文件 | 类型 | 状态 | 作用 |
|---|---|---|---|
| `test_mt6835_crc.c` | 主机测试 | 当前 | 磁编解码：边界角度、独立逐位 CRC 参考、全部单比特损坏、传感器故障状态、历史真实帧 |
| `test_justfloat.c` | 主机测试 | 当前 | 两种传输的帧字节：精确帧、参数单次求值、16 通道上限、NaN、发送被拒 |
| `test_app_usb.c` | 主机测试 | 当前 | 真实 `app.c` + `foc.c` + `control.c`：64 字节/15 float 完整帧、头字位打包、`Iq`/`rpm`/`pos`/`zero`/`hello` 解析与拒绝、CR/LF/CRLF、拆包/粘包、UART/USB 独立组行、会话切换、2,000 帧/秒分频、10 A/s 斜坡与 motion 指令、外环参考 |
| `test_control_pid.c` | 主机测试 | 当前 | 真实 `control.c` + 一阶被控对象：速度跟踪（±）、输出限幅与抗饱和、目标保持、多圈位置收敛与反向、`zero`、`stop` 复位 |
| `test_usb_queue.c` | 主机测试 | 当前 | 直接包含生产 `bsp_usb.c`：20,000 帧逐字节比对、BUSY 重试、缓冲所有权、环形/计数器回绕、溢出锁存、复位统计、RX 背压（用 `usb_stubs/` 替代 CDC 回调） |
| `test_foc_recalibration.c` | 主机测试 | 当前 | 校准状态机回归：零偏采集 → `foc_calibrate()` → 对齐 → `FOC_SAVE`，方向判定与 600 转滑行 |
| `capture_usb.py` | PC 脚本 | 当前 | 完整 2 kHz 流长跑校验：DTR 会话排空、帧尾对齐、`seq` 差 10 与 µs 差 500 的连续性、速率 1,980..2,020 帧/s |
| `foc_stub.c` | 夹具 | 当前 | 给 `test_control_pid.c` 用的最小 `foc.c` 状态机替身，不链接完整电流环 |
| `usb_stubs/usbd_cdc_if.h` | 夹具 | 当前 | 只给 `test_usb_queue.c` 用的最小 CDC/USBD 声明 |
| `legacy/test_foc.c` | 主机测试 | 历史 | 电压模式 FOC 数学、缓升、窗口与校准仿真，见下 |
| `legacy/test_foc_commands.c` | 主机测试 | 历史 | 电压模式命令、关断、校准记录校验 |
| `legacy/capture_foc.py` | PC 脚本 | 历史 | 48 字节 / 11 float 电压模式协议记录与 `run <V>` 试验 |

针对台架的工况库、单一完整字段表、归档与报告在 [tools/bench/](../tools/bench/README.md)；
`capture_usb.py` 只做完整流的长跑校验，不产生数据集；`tools/bench/foc_capture.py` 下载 20 kHz FOC3 RAM 采集并输出 CSV。

## 可重复的主机测试

在仓库根目录执行（主机 GCC，需要 `-lm`）：

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Hardware/mt6835 tests/test_mt6835_crc.c App/Hardware/mt6835/mt6835.c -lm -o build/test_mt6835_crc.exe
./build/test_mt6835_crc.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Protocols/JustFloat -I App/Hardware/bsp tests/test_justfloat.c -o build/test_justfloat.exe
./build/test_justfloat.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Control -I App/FOC -I App/Protocols/JustFloat -I App/Hardware/bsp -I App/Hardware/mt6835 tests/test_app_usb.c App/Control/app.c App/Control/control.c App/FOC/foc.c -lm -o build/test_app_usb.exe
./build/test_app_usb.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Control -I App/FOC -I App/Hardware/bsp -I App/Hardware/mt6835 tests/test_control_pid.c tests/foc_stub.c App/Control/control.c -lm -o build/test_control_pid.exe
./build/test_control_pid.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I tests/usb_stubs -I App/Hardware/bsp tests/test_usb_queue.c -o build/test_usb_queue.exe
./build/test_usb_queue.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/FOC -I App/Control -I App/Hardware/bsp -I App/Hardware/mt6835 tests/test_foc_recalibration.c App/FOC/foc.c App/Control/control.c -lm -o build/test_foc_recalibration.exe
./build/test_foc_recalibration.exe
```

解析器自检无需硬件：`python tools/bench/selftest.py`（帧字节往返、错位重同步、
丢帧检测、损坏拒绝、通道表、归档往返、工况限值）。

2026-09-23 本机实测：六个 C 程序与 `selftest.py` 均构建通过（C 用
`-Wall -Wextra -Werror` 无警告）并打印 PASS。它们不属于固件 CMake，不参与
Debug/Release 构建。

## 历史资产（`tests/legacy/`）

三个文件针对已删除的电压模式 API，**不能**对当前电流模式固件编译或连接使用：

- `legacy/test_foc.c`：`foc_step` 现需 5 个参数（旧代码给 2 个）、`foc_modulate` 现需 4 个参数（旧代码给 5 个）、
  `foc_run` 已不存在。
- `legacy/test_foc_commands.c`：同样的 `foc_step` 参数不匹配；`Set <V>` / `run <V>` 电压命令入口已移除
  （现只接受 `Iq <A>`、`stop`、`cal`、`clear`），且 UART 帧断言针对旧的 24 字节帧（现为 64 字节）。
- `legacy/capture_foc.py`：解析 48 字节 / 11 float 帧，只用于旧协议记录。

`App/Hardware/bsp/bsp_motor_record.h`（`record_t` / `checksum()` / `record_valid()`）仍在，但旧命令语义不适用。
历史结论保留在 [FOC_TEST.md](FOC_TEST.md)。

## 文档

| 文档 | 覆盖内容 |
|---|---|
| [USB_TEST.md](USB_TEST.md) | USB FS CDC：64 字节 / 15 float 2 kHz 控制日志、VOFA+ 设置、USB 命令、64 KB 队列、历史短测/长测/CPU 插桩 |
| [UART_TEST.md](UART_TEST.md) | USART2 2 Mbps：64 字节 / 15 float 20 Hz 安全状态帧、串口命令、驱动约束、波特率阶梯与历史长测 |
| [SAMPLING_TEST.md](SAMPLING_TEST.md) | 采样链路：TIM8 + 双 ADC + SPI/DMA 调度、ADC 采样时间优化、20 kHz 周期与 CPU 口径、MT6835 编码器与故障注入 |
| [CUBEMX_TEST.md](CUBEMX_TEST.md) | CubeMX 配置与 USB 集成：6.15.0 两次生成的工具/时钟/引脚设置、生成前后外设 SHA256 核对、USER CODE 保留、日志诊断分类、构建占用与未执行边界 |
| [FOC_TEST.md](FOC_TEST.md) | 电机控制：2026-09-20 电压模式历史记录（命令、帧、校准、Flash 记录、实测），当前电流模式见 `App/README.md` |

## 原始记录位置（本机，不入版本库）

| 目录 | 内容 |
|---|---|
| `build/UartStress/results/` | 2026-09-18 波特率阶梯、3.5 Mbps 长测、过载与尾包 JSON |
| `build/bench_debug/20260921_uart/`、`20260921_vofa/` | 串口命令失效核验、VOFA/Debug 实时路径修复 |
| `build/bench_debug/20260922_1137_usb/`（旧记录）、`20260922_1232_usb/`（本轮） | USB 压力测试与 900 秒长测（`summary.json`、`endurance_900s.*`） |
| `build/bench_debug/20260922_1338_compare/` | UART / USB 同帧 CPU 对比（`RESULT.md`、`summary.json`） |
| `build/bench_debug/20260922_1418_usb_current/` | 当前电流模式 USB 布局单向/双向实测 |
| `build/bench_debug/20260921_1431_noise/` | 无负载自动化调试、编码器二次谐波补偿对照 |
| `build/adc_optimization/` | ADC 母线采样时间优化（`*_summary.json`、`final_rate.json`、`final_registers.json`） |
| `build/refactor/` | 重构前备份、插桩源、板上寄存器快照、最终串口 JSON |
| `build/foc/`、`build/foc_analysis/`、`build/current_audit/`、`build/current_iteration/` | 电压模式实测、电流内环报告、电流采样核验与响应迭代 |
| `build/bench_debug/20260921_log_analysis/` | 外部日志分析（13 次 `FOC_BUS` 保护停转） |

## 其他一次性板级记录

CAN1：临时固件静默回环验证——20 帧入队后保留前 15 帧、丢弃计数 5；顺序与内容正确；标准、扩展、远程帧通过；
非法 DLC/ID 被拒绝。测试后恢复 NORMAL，最终固件不含回环与自测。未验证外部 CAN 收发器及总线 ACK，
此后没有新的 CAN 记录（当前接口见 `App/README.md`）。

## 约定

- 每个文档区分**当前结论**与**历史测量**；历史数字原样引用，不重新解释。
- 主机测试是 mock 环境：不验证中断抢占、USB 枚举和真实电机时序，不能替代板级验收。
- DWT / ISR 测量含函数序言、尾声和统计指令的开销；`motor_work_max` 是采样链路墙钟跨度（含 SPI 等待与被抢占时间），
  不是纯 CPU 占用，两者不可直接比较。
- 未做示波器测量、未标定绝对电流/电压精度、未做带载长时间试验的结论，一律写为"未验证"。
