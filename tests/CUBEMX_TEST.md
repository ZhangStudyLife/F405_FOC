# CubeMX 配置与 USB 集成验证

本文记录初始 CubeMX 集成。后续实机压力测试、64 KB CCM 队列及 4 KB 批量优化结果
见 [USB_TEST.md](USB_TEST.md)；下方“未执行”仅描述初始集成阶段。

## 实际工具与配置

- STM32CubeMX 6.15.0（本机安装记录 6.15.0-RC4），通过内置 Java quiet CLI 实际生成两次。
- IOC 固件包 STM32Cube FW_F4 V1.28.3；工程 HAL 源码版本宏为 1.8.5，不能将包版本视作 HAL 版本。
- GCC arm-none-eabi 13.3.1；CMake/Ninja 构建。
- MCU STM32F405RGT6 / LQFP64；HSE 8 MHz，PLLM=8、PLLN=336、PLLP=2、PLLQ=7。
  SYSCLK/AHB=168 MHz，APB1=42 MHz，APB2=84 MHz，USB=336/7=48 MHz。
- USB PA11 DM / PA12 DP，AF10，无上下拉。CDC FS device，VBUS sensing 关闭，DMA/低功耗关闭。
  硬件文档确认 USB VBUS 未接入 MCU；不占用 PA9，不将母线 ADC 的 VBUS_S 当成 USB VBUS。
- OTG_FS IRQ priority=7；数据/命令 bulk 端点由 ST CDC 栈处理。原生 USB 与 UART 独立。

## 已有外设核对

生成前后 `adc.c/tim.c/spi.c/gpio.c/usart.c/dma.c/can.c` SHA256 全部一致。
本次不改变这些外设的初始化或运行时 BSP 设置：

- TIM8 固定 PC6/7/8、PA7/PB0/PB1。168 MHz、PSC=0、ARR=4200，中心对齐：
  168 MHz / (2 × 4200) = 20 kHz；DTG=84 在线性编码区，84/168 MHz=500 ns。
- 生成代码仍为双 ADC injected 配置，ADC2 USER CODE 清 JAUTO 保留。
  实际运行由已有 `bsp_adc_start()` 设置双规则同步、TIM8 TRGO 触发和 DMA2 Stream0，未改动。
- SPI3 PC10/11/12 Mode 3 / 10.5 MHz；PA0 编码器 CS 初始高保持。
- USART2 PA2/PA3 的生成设置不变；已有 BSP 仍在运行时设 2 Mbps，TX DMA1 Stream6/Channel4。
- 原采样/编码器 IRQ=1；TIM8 故障、ADC 错误及运行时 UART RX IRQ=0；USB IRQ=7 不抢占它们。
- 原校准 Flash 保留区与链接脚本未改变。

## USER CODE 与独立代码

`USB_DEVICE/App/usbd_cdc_if.c` 中增加 CDC 初始化/释放、DTR、line coding、RX 与完成回调。
`usbd_cdc_if.h` USER CODE 将生成的临时 RX/TX 缓冲覆盖为 64/4 字节，实际 TX 数据由 BSP 队列持有。
`USB_DEVICE/Target/usbd_conf.c` USER CODE 处理挂起失去连续性的锁存。
独立 BSP 位于 `App/Hardware/bsp/bsp_usb.c/.h`，通过顶层 CMake 注册；未手改生成的 CMake。
USB HAL FIFO 服务和中间件在 Debug 也启用 -O3，已检查实际编译命令。
第二次生成后上述 USER CODE 均仍存在，生成后重新构建通过。

## 生成日志及诊断分类

备份：`validation/usb-before/`（含原 IOC、Core、CMake、链接脚本）。
CLI 脚本：`validation/generate.mx`；日志：`validation/usb-generate.log`、`validation/usb-regenerate.log`。
两次生成均结束于 `OK / Bye bye`。

日志存在本机第三方 PDSC 版本不支持、虚拟 RIF 引脚无法用于本 MCU、加载阶段 DMA 参数未设置的提示。
它们不能当作“日志全无错误”：最终 IOC 无 RIF 引脚且 key 无重复；实际 UART DMA 初始化完整，
与备份逐字节一致。未出现实际 Clock Configuration Error / Pinout Configuration Error；
PA11/12、48 MHz、禁用 VBUS、IRQ 及 HAL PCD 生成结果均已检查。
CubeMX 自动归一化了 IOC 的 IP/引脚顺序和时钟来源字段，实际 HSE PLL 生成代码保持原值。

## 结果与边界

| 检查 | 实际结果 |
|---|---|
| Debug | 通过，RAM 23,944 B，FLASH 68,600 B |
| Release | 通过，RAM 23,944 B，FLASH 56,648 B |
| Release + FOC_CAPTURE | 通过，RAM 122,280 B，FLASH 57,896 B |
| USB 队列主机测试 | 通过：20k 帧、BUSY、延迟完成、字节/计数回绕、溢出、复位、RX 背压 |
| JustFloat 主机测试 | 通过：UART/USB 字节格式、单次求值、通道上限、拒收 |
| PC 校验脚本 | Python 语法与命令行入口通过；未连接板卡运行 |
| 烧录/枚举/长期吞吐/最坏 ISR 时长 | 未执行，不能据主机测试宣布硬件达标 |

默认 USB 帧是 8 个数据 float 加帧尾，20 kHz 目标 720,000 B/s；16 KB 缓冲约 22.7 ms。
仅由软件不能保证主机任意暂停时无限期无丢失。满队列和挂起会显式锁存采集失败，
停止继续入队，避免掩盖缺失；这不是已实现任意条件无损的声明。
板上验收与 PC 序号校验命令见 [USB_TEST.md](USB_TEST.md)。
