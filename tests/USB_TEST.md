# USB 电流环调试

STM32F405 PA11/PA12 使用 USB FS CDC（12 Mbit/s，无 DMA）。每次 20 kHz 采样发送
8 个小端 float32，加 JustFloat 帧尾 `00 00 80 7F`，共 36 B，持续 720,000 B/s。

| 通道 | 数据 | 单位/含义 |
|---|---|---|
| 0 | `motor_sample_us` | μs；TIM5 1 MHz，第一 rank ADC DMA 中断入口时间 |
| 1 | `foc.command` | A；已接受的目标 Iq，stop/故障后归零 |
| 2 | Ib | A；本次 B 相 ADC 电压减零偏、乘 50，无滤波 |
| 3 | Ic | A；本次 C 相 ADC 电压减零偏、乘 50，无滤波 |
| 4 | `foc.id` | A；本次 d 轴反馈，无低通 |
| 5 | `foc.iq` | A；本次 q 轴反馈，无低通 |
| 6 | `foc.uq` | V；含前馈及限幅的 q 轴输出，用于下一 PWM 周期 |
| 7 | `mt6835_angle_deg` | °；编码器单圈机械角，0..360 |

Ib/Ic 校零完成前为 NaN，名义 0.020 V/A 换算仍需实际电流标定。可计算 `Ia=-Ib-Ic`。
本布局不同时发送三相 PWM、Ud 或纯 PI 积分项；原 UART 2 kHz 帧仍含三相 PWM。

时间戳不是 USB 到达时间，也不是序号乘 50；它晚于模拟保持时刻，包含中断延迟。
按 `2^24=16,777,216 μs` 回绕，float32 能精确表示每个整数；相邻模差约 50 μs：
`(current-previous)&0xFFFFFF`。长时间轴需上位机展开，复位/重启采样会重置计时。
编码器内部延迟未标定，不能将机械角与 ADC 当成绝对同时测量。

## VOFA+ 设置和命令

1. 选择 Serial/串口和原生 USB 端口（本机 COM17，以实际枚举为准），2000000 baud、8N1、无流控。
2. 协议选择 JustFloat，开启 DTR 后连接。CDC 波特率不决定 USB 实际吞吐。
3. 若版本支持采样间隔，设 50 μs / 0.00005 s；否则按点数换算。显示刷新可设 30–60 Hz。
4. 叠加通道 1/5 观察 Iq 目标/反馈，另看 Id、Uq、Ib/Ic；不要把回绕时间戳直接用作无限递增 X 轴。

发送文本 `Iq 0.20` 并附加实际 CR（0x0D），也支持 LF/CRLF。不要发送字面量 `\r`；
HEX 等价字节：`49 71 20 30 2E 32 30 0D`。`stop` 加回车立即关断；运行中的 `Iq 0.00` 只设零目标。
接受整数或最多两位小数、范围 ±5 A，保留原有校准/状态/故障门槛。负 Iq 是负转矩，不保证立即反转。
USB/UART 独立组行、共用命令处理，不向波形流混入文字 ACK；拒绝计数见 `app_command_rejected`。

## 驱动约束

`usb_justfloat(...)` 发送显式通道；`uart_justfloat(...)` 额外带毫秒时间戳，两者返回整帧是否成功入队。
USB 仅允许 `app_sample()` 一个生产者；前台运行 `bsp_usb_poll()` 和 `bsp_usb_read()`。
接收端每包 64 B，应用消费完前保持 NAK；命令每轮最多解析一包。

64 KB CCM 环形队列在 USB 完成（含 ZLP）后释放空间；达到 4 KB 或距上次提交 10 ms 时发送，
每批最多 4 KB。没有等待发送完成的循环，也不在发送路径全局关中断；CPU 仍需搬运 USB FIFO。
USB IRQ 优先级 7，低于采样/编码器 IRQ 1。TIM5 由 BSP 独占，不能另作他用。

枚举且 DTR=1 才入队；DTR=0 停止入队，旧数据继续排空。重新打开是新会话，可能先收到旧尾部；
校验脚本在会话开始/结束时关闭 DTR 并排空 250 ms，不将跨会话间隔算成连续采集丢帧。
队列满时拒收整帧并锁存故障，已入队数据仍可发完；USB 挂起也锁存。需复位/重新枚举才能清除。
`g_usb_stats` 记录完成字节、拒收、复位丢弃、队列峰值、接收字节、会话/中断次数和溢出标志。

64 KB 仅相当于约 91 ms 缓冲，不能在主机休眠/停读/拔线时无限保存数据。
USB FS bulk 理想上限约 1,216,000 B/s，实际吞吐还取决于主机、软件和中断负载。
USB ACK 不证明 VOFA 已保存全部样本；校准擦写、复位或采样恢复不属于连续采集工况。

## 验证

```powershell
gcc -std=c11 -Wall -Wextra -Werror -O2 -I tests/usb_stubs -I App/Hardware/bsp tests/test_usb_queue.c -o build/test_usb_queue.exe
./build/test_usb_queue.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Protocols/JustFloat -I App/Hardware/bsp tests/test_justfloat.c -o build/test_justfloat.exe
./build/test_justfloat.exe
gcc -std=c11 -Wall -Wextra -Werror -O2 -I App/Control -I App/FOC -I App/Protocols/JustFloat -I App/Hardware/bsp -I App/Hardware/mt6835 tests/test_app_usb.c App/Control/app.c App/FOC/foc.c -lm -o build/test_app_usb.exe
./build/test_app_usb.exe
python tests/capture_usb.py COM17 --seconds 60
```

主机脚本需要 pyserial，执行前关闭 VOFA。检查 45..55 μs 时间戳模差、约 20,000 帧/s，
并核对 MCU USB/ADC/编码器/控制时序计数；至少 30 秒覆盖时间戳回绕。主机测试不能替代 PWM 截止和模拟测量。
本布局已在电机关闭工况通过单向 60 s 和约 64 s 双向测试，后者 1,271,841 帧、4 次时间戳回绕、
间隔 49..51 μs；接收 1,000 条零目标命令和 5 条预期拒绝的非法命令，通信/采样/控制时序错误为零。
非零目标由主机测试验证；这些结果不代表带转矩调参或 VOFA 长时间绘图已验收。
