# STM32F405 M1 FOC CubeMX 配置报告

验证日期：2026-09-11。工程目录：`E:\405_FOC\405_FOC`。

已由本机 STM32CubeMX 实际生成 HAL + CMake 工程，并通过 ARM GCC/Ninja 干净构建。重新加载、保存、生成后 `.ioc` 字节完全一致，ADC2 USER CODE 补充保留。没有实现 FOC 算法，没有下载到板卡，也没有进行带电测试。主程序只初始化外设，不启动 PWM 或 ADC，故上电运行此骨架不会主动使能功率输出。

配置入口是 [405_FOC.ioc](405_FOC.ioc)，原文件备份为 [405_FOC.ioc.original.bak](405_FOC.ioc.original.bak)。唯一的 CubeMX 表达限制及寄存器补充见第 15 节。所有 RCC/GPIO/TIM/ADC/SPI HAL 初始化及 CMake 文件均由 CubeMX 生成，未手工改写自动生成区。

## 1. CubeMX 版本

- STM32CubeMX **6.15.0**，数据库 **DB.6.0.150**，Project Generator **4.7.0-B52**。
- 可执行文件：`C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeMX\STM32CubeMX.exe`。
- 命令行使用该安装自带 Java 21.0.3 运行同一个 EXE/JAR，`-q` 执行 [generate.mx](validation/generate.mx)。Windows 项目路径采用反斜杠，避免此版本对正斜杠绝对路径的重复拼接问题。
- 参数名由当前安装的 `db/mcu/IP/*_Modes.xml`、`db/mcu/config/*_Configs.xml`、实际 CLI 保存和重载结果确定。ADC 注入序列最终使用实际模式参数 `Channel / InjectedRank / SamplingTime-0#ChannelInjectedConversion`。

## 2. CubeF4 HAL 版本

- Firmware package：**STM32Cube FW_F4 V1.28.3**，实际已安装于 `D:\stm32f103vet6_selfstudy\STM32Cube_FW_F4_V1.28.3`。
- HAL 驱动版本：**1.8.5**，由生成的 `stm32f4xx_hal.c` 版本宏确认。包版本与 HAL 驱动版本不是同一个编号。
- 使用项目内复制的 HAL/CMSIS，构建无需读取上述包目录；再次生成仍需要 CubeMX 与对应固件包。

## 3. MCU 与固定硬件网络

STM32F405RGT6 / STM32F405RGTx，LQFP64，Flash 1 MiB，SRAM 128 KiB，CCM 64 KiB。

网络依据当前目录原有原理图渲染 `../tmp/pdfs/odrive_f405-1.png`、`odrive_f405-3.png`，以及当前 CubeMX MCU 引脚数据库确认。未重新分配硬件网络。

| 网络 | MCU 引脚 | 使用功能 |
|---|---|---|
| M1_AH / M1_AL | PC6 / PA7 | TIM8_CH1 / TIM8_CH1N，AF3 |
| M1_BH / M1_BL | PC7 / PB0 | TIM8_CH2 / TIM8_CH2N，AF3 |
| M1_CH / M1_CL | PC8 / PB1 | TIM8_CH3 / TIM8_CH3N，AF3 |
| M1_SO1 | PC3 | ADC1_IN13；原理图 B 桥臂低侧分流 |
| M1_SO2 | PC2 | ADC2_IN12；原理图 C 桥臂低侧分流 |
| SPI_SCK / MISO / MOSI | PC10 / PC11 / PC12 | SPI3，AF6 |
| GPIO1 / MT6835 CS | PA0 | GPIO 输出；CubeMX 名称 PA0-WKUP |
| SWDIO / SWCLK | PA13 / PA14 | Serial Wire |

M0、RTC、LSE、JTAG、USB、CAN 等未启用。M1_TEMP（PA4/IN4）、VBUS_S（PA6/IN6）本版未配置，减少背景采样对调试的干扰。

## 4. Clock Tree

| 项目 | 最终值 |
|---|---|
| 外部晶振 | HSE 8 MHz，Crystal/Ceramic，非 bypass |
| PLL | HSE，M=8、N=336、P=2、Q=7 |
| PLL 输入 / VCO | 1 MHz / 336 MHz |
| SYSCLK / HCLK | 168 MHz / 168 MHz |
| APB1 / APB1 timer | 42 MHz / 84 MHz |
| APB2 / APB2 timer | 84 MHz / **168 MHz** |
| PLLQ 输出 | 48 MHz |
| ADC 共用时钟 | PCLK2/4 = **21 MHz** |
| Flash / 电压档位 | 5 wait states / Scale 1 |

已检查 `SystemClock_Config()` 实际使用 HSE，不是原工程的 HSI。HAL 配置头的 HSE_VALUE 为 8000000。

`.ioc` 保留 CubeMX 自动保存的未使用时钟缓存字段；其中未启用的 RTC HSE 分频设为 /8，使派生缓存为 1 MHz，消除其参数上限告警。没有生成 RTC 初始化或使能 RTC/LSE。

## 5. TIM8 最终配置

- Internal Clock，Slave Mode Disable；没有 `HAL_TIM_SlaveConfigSynchro()`，不使用 Gated/Trigger slave。
- Center-Aligned Mode 1，PSC=0，ARR=4200，CKD=DIV1，RCR=0，ARR preload Enable。
- CH1/2/3 均为 PWM mode 1，三对互补输出，共用 TIM8 计数器。主输出及互补输出均为高有效、idle reset，初始 CCR1/2/3=0。
- CH4 为 Output Compare / Timing、No Output，CCR4=4100；无 CH4 GPIO，无 MOSFET 连接。
- BDTR dead time=84；Break Disable，AutomaticOutput Disable，Lock OFF，OSSR/OSSI Disable。
- 初始化不调用任何 PWM/PWMN/OC/Base Start。初始零占空比不等于“互补两管同时关断”：若以后主动使能互补输出，PWM1 零占空比对应高侧关、低侧导通。当前骨架通过不启动输出来保持未使能状态。

## 6. 实际 PWM 频率计算

RM0090 §17.3.1 的中心对齐序列是 `0…ARR-1, ARR…1`，一周期共 `2×ARR` 个计数时钟：

```text
fPWM = 168000000 / ((PSC+1) × 2 × ARR)
     = 168000000 / (1 × 2 × 4200)
     = 20000 Hz，周期 50 µs
```

ARR=4199 会得到约 20004.763 Hz，因此按题目允许的 off-by-one 修正使用 **4200**。该值为时钟标称值计算，未用示波器实测晶振误差。

## 7. Dead-Time 实际时间计算

CKD=DIV1，因此 tDTS=1/168 MHz=5.952381 ns。DTG=84=0x54，最高位为 0，属于线性编码区：

```text
DT = DTG[7:0] × tDTS = 84 / 168 MHz = 500 ns
```

没有把“500”当成寄存器值。依据 RM0090 §17.4.18 BDTR.DTG：线性区 0…127；其余区间分别为 `(64+n)×2`、`(32+n)×8`、`(32+n)×16` 个 tDTS。

## 8. ADC1 配置

- 主 ADC，PC3 / channel 13 / M1_SO1，12-bit、右对齐。
- 注入序列 1 个转换、rank 1，28 cycles，offset=0。
- External injected trigger：TIM8_CC4，Rising edge。
- Continuous、discontinuous、auto-injected、DMA continuous requests 均关闭。
- CubeMX 同时保留相同引脚的 regular rank 1（28 cycles、software trigger），主程序不启动 regular 转换；FOC 主采样只使用 injected。

## 9. ADC2 配置

- 从 ADC，PC2 / channel 12 / M1_SO2，12-bit、右对齐。
- 注入序列 1 个转换、rank 1，28 cycles，offset=0。
- 转换启动由 ADC1 多 ADC 硬件同步分发，不独立设置第二个外部触发。
- Continuous/discontinuous 关闭；同样存在未启动的 regular rank 1。
- CubeMX 生成的 AutoInjected=ENABLE 已在 USER CODE 中清除 JAUTO，最终由主 ADC 同步触发，见第 15 节。

## 10. ADC 同步模式

实际生成 `HAL_ADCEx_MultiModeConfigChannel(&hadc1, ...)`，`Mode=ADC_DUALMODE_INJECSIMULT`，对应 ADC_CCR.MULTI=00101。不是两个独立 ADC 各自启动。两个 injected 序列长度及采样时间一致。

依据 RM0090 §13.9.1，ADC1 的 JEXTSEL 触发同步传送至 ADC2，结果分别存入 ADC1_JDR1、ADC2_JDR1。生成的 TwoSamplingDelay=5 cycles 是 common 配置字段，对本 injected simultaneous 模式不引入两相串行采样间隔。没有配置多 ADC DMA。

21 MHz 低于 F405 ADC 36 MHz 上限。28 cycles 的采样窗约 **1.333 µs**，加 12-bit 转换的 12 cycles 后约 **1.905 µs**，不含外部触发同步延迟。原理图输出为运放后 100 Ω / 2.2 nF（τ≈0.22 µs），选择 28 cycles 留出初版余量，未采用 3 cycles。运放型号标注“以实物为准”，最终建立时间仍需台架验证。

## 11. ADC 触发源与采样位置

RM0090 ADC_CR2.JEXTSEL[3:0] 明确规定 **1110=Timer 8 CC4 event**，CubeMX F405 可选项及 HAL 宏 `ADC_EXTERNALTRIGINJECCONV_T8_CC4` 一致。F405 injected 不应改成并不存在于其该选择表中的 TIM8_TRGO。本项目可直接采用 CC4，无需第二个定时器桥接。

TIM8_CH4 timing compare，CCR4=4100；Center-Aligned Mode 1 的输出比较事件在向下计数方向产生，因此每完整 PWM 周期对应一次采样事件，标称 20 kHz。未开启 CC4 或 TIM8 Update 中断。以从 CNT=0 向上开始计算，首次向下匹配位于 `(2×4200−4100)/168 MHz≈25.595 µs`。

这是可调整的初始低侧电流采样时刻，接近计数器顶部的 PWM1 低侧零矢量区。后续 SVPWM 必须保证 B/C 两个低侧开关在采样窗内都稳定导通，并为死区、运放建立时间和整个 ADC 采样窗留出余量；高调制度时需要限制占空比或调整 CCR4。固定触发配置本身不保证所有占空比下的双分流可观测性。

本版未 arm ADC/TIM8，所以不会自行进入 ISR。后续启动应先使能从 ADC2，再用 ADC1 injected interrupt start 等待硬件事件，最后启动 TIM8 的采样时基；只对 ADC1 开 JEOC 中断，并在其回调内读取两路 JDR。功率输出使能应在零偏校准、有效采样窗和控制逻辑就绪后单独完成，不要把普通 while 软件启动作为采样时基。

## 12. SPI3 最终配置

Full-Duplex Master，Motorola，8-bit，MSB first，software NSS；CPOL=High、CPHA=2Edge，即 Mode 3。PCLK1=42 MHz，prescaler=/4，SCK=**10.5 MHz**。CRC Disable，无 DMA、无 SPI IRQ。

已查 MagnTek 官方 [MT6835 Rev.1.3 数据手册](https://www.magntek.com.cn/upload/pdf/202407/MT6835_Rev.1.3.pdf)，确认 Mode 3；官方 [产品资料](https://www.magntek.com.cn/115/) 确认 SPI 上限 16 MHz，因此 10.5 MHz 未超限。

## 13. MT6835 CS 配置

PA0-WKUP，User Label=`MT6835_CS`，普通 GPIO Output Push-Pull，No Pull，Low GPIO speed。生成代码先写 `GPIO_PIN_SET`，后切换为输出；不占用 SPI3 硬件 NSS。这里保证的是 GPIO 初始化后的 HIGH；MCU 复位到执行初始化之前仍遵循芯片复位引脚状态。

## 14. NVIC 配置

NVIC_PRIORITYGROUP_4。ADC_IRQn=抢占优先级 **0** / 子优先级 0；SysTick=15。共享 `ADC_IRQHandler()` 由 CubeMX 生成，调用两个 HAL ADC handler；实际 JEOC 中断使能属于之后的 arm/start 动作，本版尚未启动。未开启 TIM8 Update、TIM8 CC、SPI3、DMA 等应用中断。

## 15. CubeMX 无法直接实现、需要 USER CODE 补充的部分

**只有 ADC2 JAUTO 清除这一处。** 当前 F4 ADC XML 在 ADC2 multimode 从属模式下把 ExternalTrigInjecConvEdge 隐藏并置 null；相关条件导致 AutoInjected 默认启用。尝试选择 None 会得到 ADC2 not ready，显式设置隐藏触发边沿也不能改变最终代码。

保留 `.ioc` 的 Dual Injected Simultaneous 配置，`Core/Src/adc.c` 中仅在 `USER CODE BEGIN ADC2_Init 2` 内补充：

```c
CLEAR_BIT(hadc2.Instance->CR1, ADC_CR1_JAUTO);
```

它在 HAL ADC2 初始化后禁止 regular→injected 自动注入，不修改 ADC1 触发、多 ADC 模式、通道或采样时间。该补充属于题目允许的生成器表达限制例外；不能仅复制 `.ioc` 到空目录就期待这行自动出现，移植时须保留 USER CODE。`KeepUserCode=true`，本工程实际重新 Generate Code 后已验证该代码保留，ADC 源文件文本完全一致。

除了上述一位，没有手写任何初始化函数，没有加入 FOC、电机参数计算、编码器读角度驱动或自动启动逻辑。

## 16. CMake build 结果

工具：CMake **4.3.2**、Ninja **1.13.2**、GNU Tools for STM32 **13.3.1**。编译器为 `D:\DevEnv\GNU-tools-for-STM32\bin\arm-none-eabi-gcc.exe`，目标 Cortex-M4 / fpv4-sp-d16 / hard-float。

在工程目录执行：

```powershell
& 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeMX\jre\bin\java.exe' -jar 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeMX\STM32CubeMX.exe' -q .\validation\generate.mx
cmake --preset Debug
cmake --build --preset Debug --clean-first
python .\validation\verify_config.py
```

最终完整构建 **31/31 成功**，输出 `build/Debug/405_FOC.elf`；FLASH 14328 B、RAM 1888 B、CCMRAM 0 B。GCC 编译与链接 **0 warning / 0 error**。在 VS Code 中可直接选择生成的 Debug/Release CMake preset；本次实际验证 Debug。

证据：[生成日志](validation/cubemx-generation.log)、[configure 日志](validation/cmake-configure.log)、[build 日志](validation/cmake-build.log)、[外设审计](validation/config-audit.log)、[重复生成检查](validation/regeneration-check.txt)。审计验证 MCU/时钟/PWM/ADC/SPI/CS/NVIC 与未启动状态，未进行板上寄存器读取或波形测量。

## 17. 所有 warning / error

**工程实际 Clock Configuration Error=false、Pinout & Configuration Error=false，无未解决的硬件引脚冲突、时钟冲突或外设参数错误。不能声称整个 CubeMX 环境日志零错误。** 最终日志全部 179 条告警/错误相关记录（含重复项）原样提取至 [cubemx-diagnostics.txt](validation/cubemx-diagnostics.txt)。

仍存在的工具/环境诊断：

1. `Pin19 (VP_RIF_VS_RIF1) cannot be retrieved for this MCU`：已检查安装的 CubeMX `Mcu.loadConfig` 实现。它只在 DIE505 检测 RIF 是否存在，却在其他 die 上也执行缺失 RIF 的恢复逻辑，导致 DIE413/F405 的虚拟引脚告警。最终 `.ioc` 19 个引脚全部有效且没有 RIF 项；它不是本板网络未分配。未修改 CubeMX 程序或伪造 RIF 引脚来压制日志。详见 [工具行为记录](validation/tool-loader-findings.txt)。
2. 本机第三方 CMSIS PDSC 索引版本不支持：X-CUBE-SMBUS 2.1.1、X-CUBE-WBA 1.1.0、FP-SNS-STAIOTCFT 2.0.0、X-CUBE-ST67W61 1.3.0、X-CUBE-BLEMGR 4.2.0。项目未使用这些包。
3. 其他第三方包缺少 IP mode、condition id、component mode、重复组件、更新器锁/忙、数据库路径覆盖、RTOS schema、Java prefs 权限等安装环境告警；完整名称与重复记录均保留在上述诊断文件，不影响已验证的 F4 HAL 生成和构建。

执行中已解决的问题也保留在 `validation/cubemx-generation-attempt*.log`：ADC2 None 配置导致 not ready（按第15节处理）；正斜杠 Windows 路径导致 sysmem/syscalls 路径重复（改生成脚本路径格式）；未使用 RTC 的派生频率缓存超过字段上限（设置未启用源的 /8 分频缓存）。最终生成未再出现这些问题。

主要芯片依据为 ST [RM0090 Rev.22](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405415xx-stm32f407417xx-stm32f427437xx-and-stm32f429439xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)：§13.9.1 injected simultaneous、ADC_CR2.JEXTSEL、§17.3.1 center-aligned、§17.4.18 BDTR。已下载核对原文，副本位于 `../tmp/pdfs/RM0090.pdf`。
