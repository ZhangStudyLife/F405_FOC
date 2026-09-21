# 采样固件历史验证记录

[FOC_TEST.md](FOC_TEST.md) 为早期电压模式历史记录。当前接口见 [App/README.md](../App/README.md)。`test_foc.c`、`test_foc_commands.c` 和 `capture_foc.py` 属于历史电压模式，不能针对当前电流模式编译或连接使用。以下保留改动前采样固件结果，不代表当前 FOC 固件。

2026-09-20，STM32F405、168 MHz，ST-Link 8600A1002031363534313541。此文件替代旧 ADC/UART/编码器测试说明；历史内容可在 Git 历史和本机 `build/refactor/before.zip` 中查阅。

## 可重复的主机测试

在工程根目录运行：

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Hardware/mt6835 tests/test_mt6835_crc.c App/Hardware/mt6835/mt6835.c -lm -o build/test_mt6835_crc.exe
./build/test_mt6835_crc.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Protocols/JustFloat -I App/Hardware/bsp tests/test_justfloat.c -o build/test_justfloat.exe
./build/test_justfloat.exe
```

均通过。编码器测试覆盖边界角度、独立逐位 CRC 参考、数据/CRC 任意单比特损坏、所有传感器故障状态、历史真实采样帧。JustFloat 覆盖精确帧字节、单次求值、16 通道上限、NaN 和发送拒绝。

## 构建和最终固件

- Debug / Release 编译通过；最终 Release 已烧录、回读校验并运行。
- App 手写 C/H 从 20 文件、3173 行缩减；删除测试入口、通用包装和未使用功能，不裁剪 HAL/CMSIS。
- Release Flash（text+data）16888 → 14940 bytes；BSS 4808 → 2792 bytes。
- 最终固件 COM14、2 Mbps 连续 30 秒获得 30001 个有效位置帧，所有相邻时间戳均差 1 ms，1000 Hz；无内部错帧、NaN 或越界角度。
- ADC 错误、编码器错误、UART 拒绝/DMA/RX 错误均为 0。ADC 静态电压约 B=1.662 V、C=1.681 V、母线=4.527 V；只是数字链路检查，不是精度校准。
- SWD 确认 TIM8 PSC=0、ARR=4200、CCR4=4100、CCER 仅 CH4；六路栅极引脚均为 GPIO 推挽低。未用示波器测实际栅极电压。

## 临时插桩实测（最终固件已移除）

在 Release 两个 DMA 中断内使用 DWT 测量调用区间，前台不休眠。持续窗口内 ADC 约 158 万次，ADC/编码器错误均为 0。

| 项目 | 测量结果 |
|---|---|
| ADC DMA 完成周期 | 8391～8409 cycles，即 49.946～50.054 us |
| ADC 发布 + SPI DMA 启动 | 初次窗口平均约 0.72 us；各窗口最大 155 cycles（0.923 us） |
| SPI DMA 完成 + 解码 + 20 分频上传 | 初次窗口平均约 1.10 us；各窗口最大 415 cycles（2.470 us） |
| 两段测量区间最大值之和 | 570 cycles（3.393 us） |
| ADC IRQ 入口至编码器处理完成 | 最大 1453 cycles（8.649 us），包含 DMA 线上传输 |

此前固件 ADC ISR 最大测量区间为 1933 cycles（11.506 us），含 SPI DMA 忙等。新路径不再等待 SPI。上述区间不含完整异常进出栈、函数序言/尾声、所有统计指令及 UART/CAN 中断；不能当作精确全系统 CPU 占用，也不宣称是所有实现中绝对最低开销。

## 通信测试

临时固件 CAN 静默回环验证：20 帧入队后保留前 15 帧、丢弃计数 5；顺序/内容正确；标准、扩展和远程帧通过；非法 DLC/ID 拒绝。测试后恢复 NORMAL，最终固件不含回环和自测。未验证外部 CAN 收发器及总线 ACK。

临时固件串口回显验证：2 Mbps 下 76 字节从上位机发出、MCU 接收并完整回传，相关错误计数为 0。3.5 Mbps 同样测试产生 76 次 RX 错误，因此最终使用 2 Mbps。没有进行持续满速双工验收，应用最终不自动回显。

本机原始记录在 `build/refactor/`：重构前备份、临时插桩源、板上寄存器快照、最终串口 JSON。测试统计/回显/CAN 自测均不进入最终固件；保留两个独立主机测试用于回归。
