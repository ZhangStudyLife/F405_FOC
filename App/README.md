# 固件结构与使用

当前功能：20 kHz 同步采集 B/C 相电压、母线电压和 MT6835 机械角度；位置经 JustFloat 以 1 kHz 上传。CAN1、USART2 保留非阻塞收发接口。六路栅极保持 GPIO 低电平，尚未实现电机控制。

## 模块

| 位置 | 职责 |
|---|---|
| `Control/app.c` | 初始化、每 20 次编码器采样上传一次 |
| `Hardware/bsp/bsp_adc.c` | TIM8 采样触发、双 ADC 同步 DMA、电压换算 |
| `Hardware/mt6835/mt6835.c` | 无 HAL 的 CRC 校验及角度解码 |
| `Hardware/mt6835/mt6835_port_stm32.c` | 固定板级 SPI3/PA0 接线、DMA 启停 |
| `Hardware/bsp/bsp_can.c` | CAN1 非阻塞收发和接收队列 |
| `Hardware/bsp/bsp_uart.c` | UART DMA 发送、接收队列 |
| `Protocols/JustFloat/justfloat.h` | JustFloat 帧封装 |

应用和协议不依赖 HAL；硬件驱动不调用应用。`Core/Src/main.c` 只调用 `app_init()`，主循环休眠；`stm32f4xx_it.c` 连接各驱动和应用回调。自写代码只使用生成文件的 USER CODE 块，其他代码保持 CubeMX 所有权。

## 采样链路

TIM8 中心对齐，168 MHz、PSC=0、ARR=4200；CH4 Toggle、CCR4=4100，OC4REF 作为 TRGO，上升沿每 50 us 触发双 ADC 规则同步转换。

ADC1 rank1=PC3/IN13（B 相），ADC2 rank1=PC2/IN12（C 相）；rank2 均为 PA6/IN6（母线）。ADC 时钟 21 MHz，采样时间分别 28/15 cycles，完整转换约 3.19 us。DMA2 Stream0/Channel0 循环读取公共 CDR，每次搬运两个 32-bit 字，第二个字完成后中断。

1. ADC DMA 中断发布 `adc_sample` 三路电压，再启动 SPI DMA。
2. SPI3（Mode 3、10.5 MHz）由 DMA1 Stream5/Channel0 发送 6 字节，Stream0/Channel0 接收；CPU 不等待 SPI。
3. SPI RX DMA 完成中断校验 CRC 和传感器状态，更新 `mt6835_angle_deg`，调用 `app_sample()`。
4. 每 20 次回调提交一次位置帧，SysTick 启动 UART TX DMA1 Stream6/Channel4。

ADC 硬件转换及数据搬运不占 CPU 指令时间；电压换算、SPI 启动、CRC 解码仍需 CPU，不能称为零开销。ADC 和 SPI DMA IRQ 同为优先级 0，不互相抢占。

`adc_sample` 使用标称 VDDA=3.3 V，保留电流放大器偏置；母线倍率 41.2/2.2。前台读取完整三路快照须短暂屏蔽 DMA2_Stream0 IRQ。ADC overrun 或 DMA 错误停止采样，并置 NaN；编码器 CRC/状态/DMA 错误输出 NaN。`adc_errors`、`mt6835_errors` 只保留必要错误计数。

固定 CCR4 未验证带功率的电流采样窗口；运行电机前仍需低侧导通窗口、死区和放大器建立时间校准。

## 通信

USART2：PA2 TX / PA3 RX，**2000000 baud、8N1、无流控**。当前转换器 COM14 在 3.5 Mbps 下发送到 MCU 存在 RX 错误，2 Mbps 双向已验证。

`justfloat_send(a, b, ...)` 支持 1～16 个 float，参数单次求值。帧为小端 `float 毫秒时间戳 | float 数据... | 00 00 80 7F`。当前两通道为时间戳和单圈角度（0～360°），共 12 字节。float 时间戳在约 4.66 小时后丢失逐毫秒分辨率。

`bsp_uart_write(data, size)` 复制后立即返回；双 128-byte TX 缓冲，满时整帧拒绝。`bsp_uart_read()` 从 127-byte 有效容量接收队列非阻塞取数据。`g_uart_stats` 记录拒绝、DMA 和 RX 错误。接收使用逐字节中断，不承诺满速持续双工；不得直接混用 HAL UART 收发。

CAN1：PB8 RX / PB9 TX，1 Mbps、标准/扩展/远程帧；`bsp_can_send()` 提交至硬件邮箱，不等待 ACK；`bsp_can_recv()` 从队列取帧。队列有效容量 15 帧，满时丢新帧并递增 `can_rx_lost`。应用目前不解释 CAN/串口命令，也不自动回显。

## 构建与配置

工程根目录执行 `cmake --preset Release`、`cmake --build --preset Release`。`../download/flash.py flash Release` 自动选择匹配 F405/1MB 的 ST-Link 并校验烧录。Debug 用于调试，实时性用 Release 测量。

`.ioc` 提供基础外设/引脚初始化；最终 ADC 规则同步、TRGO 和 DMA 配置由 BSP 在启动时覆盖。重新生成不会改动 App，但必须保留 USER CODE。DMA2 Stream0、DMA1 Stream0/5 已由驱动占用，不能分配给其他设备。

验证结果见 `../tests/README.md`。旧测试入口、测速/EEPROM/多实例接口及通用 GPIO/SPI/时间包装已删除，HAL/CMSIS 原厂文件不做手工裁剪。
