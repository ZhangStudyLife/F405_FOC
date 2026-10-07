# USB CDC、UART 与 JustFloat 调试通信实施计划

日期：2026-10-08。工程：`E:\405_FOC\405_FOC`。基线：`9bfdaec`。

本文记录实施要求。USB、Debug 与 JustFloat 代码已实现，五种配置的 Debug/Release 构建及 CubeMX 再生成检查通过；板上通信和电机验收以实际测试记录为准，不能将构建通过当作实机通过。

## 1. 已确定的要求与实施边界

- 保持 20 kHz 电流环、1 kHz 速度/位置环和现有电机保护。
- 调试回传目标为 1 kHz，保留现有 20 通道，共 84 B/帧，即 84,000 B/s。
- USB 和 UART 使用相同的用户层 `write/read` 接口、相同的 JustFloat 帧；应用不操作 HAL、DMA、USB 端点或 `TxState`。
- 配置统一放在 `App/Debug/debug.h`，选择启用 USB、启用 UART，以及唯一的命令来源。启用的通道都发送遥测，只有选定通道的命令进入执行函数。
- 默认 USB 启用、UART 禁用、命令来自 USB。可以编译成 UART 单独使用、两路同时回传或全部关闭。
- 现有 `rpm`、`iq`、`pos`、`stop`、参数保存、校准和音乐命令保持语法及执行条件。
- 继续使用文本命令和 CR/LF；JustFloat 用于回传波形。波形流不混入文本 ACK。
- 拔线、DTR 关闭、主机停读不自动改变电机目标，保留当前使用语义。
- 只使用静态缓冲、普通函数和编译期条件。没有 RTOS、动态内存、传输注册表、函数指针接口或运行时通道配置。
- 不整包恢复旧 USB 工程，不恢复 CAN、stdio、旧上位机和旧测试框架。
- 保留当前 Flash 扇区 10/11 的参数及校准布局，不恢复历史链接脚本。

仓库 `AGENTS.md` 已按本次用户要求更新调试传输及回传频率。

## 2. 当前文件与目标文件

所有手写模块继续放在 `App`。CubeMX 生成文件留在生成目录。

```text
405_FOC/
├── 405_FOC.ioc
├── Core/
│   └── Src/
│       ├── main.c                    启动及主循环接入
│       ├── usart.c                   CubeMX UART 初始化
│       └── stm32f4xx_it.c            CubeMX IRQ 入口
├── Drivers/STM32F4xx_HAL_Driver/      HAL PCD、UART、USB 底层
├── Middlewares/ST/STM32_USB_Device_Library/
│   ├── Core/                        ST USB Device 栈
│   └── Class/CDC/                   ST CDC 类
├── USB_DEVICE/
│   ├── App/
│   │   ├── usb_device.c/.h          CubeMX USB 初始化
│   │   ├── usbd_desc.c/.h           CubeMX 描述符
│   │   └── usbd_cdc_if.c/.h         CubeMX CDC 接口及回调接入
│   └── Target/
│       └── usbd_conf.c/.h           HAL PCD 适配、FIFO 与 USB IRQ 配置
└── App/
    ├── Config/
    │   └── motor_config.h           已有：电机配置
    ├── Hardware/bsp/
    │   ├── bsp_uart.c/.h            已有：UART 字节收发
    │   └── bsp_usb.c/.h             新增：USB 字节收发
    ├── Protocols/JustFloat/
    │   └── justfloat.h              新增：原地补帧尾的小函数
    ├── Debug/
    │   ├── debug.h                  新增：调试配置及对外接口
    │   └── debug.c                  新增：分发、命令来源选择及组行
    └── Control/
        └── app.c/.h                 已有：电机应用、遥测内容及命令执行
```

当前 UART 用户驱动就是 `App/Hardware/bsp/bsp_uart.c/.h`，不是 `Core/Src/usart.c`。当前 `telemetry()`、命令组行和 `app_command()` 都在 `App/Control/app.c`。

新增两个 `.c`：`bsp_usb.c`、`debug.c`。JustFloat 的功能很小，只新增头文件，不为它单独建立 `.c`。调试配置直接合并到 `debug.h`，不单独建立配置头。

## 3. CubeMX 生成 USB 的步骤

1. 从当前 `.ioc` 增加 `USB_OTG_FS / Device Only` 和 USB Device CDC，不替换整个旧 `.ioc`。
2. PA11 为 DM、PA12 为 DP，使用当前硬件连接。关闭 VBUS sensing：硬件文档明确 USB VBUS 未接入板内电源树；`VBUS_S` 是电机母线采样，不能当作 USB VBUS，也不占用 PA9。
3. 保留 HSE 8 MHz、PLLM=8、PLLN=336、PLLQ=7：CPU 168 MHz，USB 48 MHz。
4. 使用 OTG_FS 的嵌入式 PHY；DMA、低功耗和本任务不使用的 SOF 回调保持关闭。
5. USB IRQ 抢占优先级设为 7，低于现有采样链。核对 `.ioc`、生成的 `HAL_NVIC_SetPriority()` 和板上 NVIC，三者必须一致。
6. 使用工程匹配的 STM32Cube F4 包生成 PCD、USB Device、CDC 和对应 CMake 集成；不从另一个版本手工混搭库文件。
7. 在生成文件的 USER CODE 块接入 `bsp_usb`，然后再生成一次，确认接入代码保留。
8. 对比生成前后 ADC、TIM8、SPI3、GPIO、DMA、UART、时钟及链接脚本，检查是否有任务之外的变化。

历史删除 USB 前，`.ioc` 写优先级 7，而 `USB_DEVICE/Target/usbd_conf.c` 写优先级 2。本次必须解决生成结果不一致，不能只看配置界面。

FIFO 在 USB 启动前配置一次，按生成的端点定义分配 RX、EP0 IN、CDC 数据 IN 和通知 IN。所有 FIFO 以 32 位 word 为单位，合计不超过 OTG_FS 的 320 words；不在枚举过程中重新分区。精确布局在生成后按实际端点检查，不能直接把旧版本的三项 FIFO 数字当作所有端点已覆盖的证据。

实际分配为 128/32/144/16 words，合计 320。CubeMX 6.15 的该模板没有 FIFO 配置之后、USB 启动之前的 USER CODE 挂点；顶层 CMake 使用 `--wrap=HAL_PCD_Start`，由 `bsp_usb.c` 在真正启动前配置一次，保证再生成仍保留该行为。

`usbd_cdc_if.h` 的 USER CODE 将 RX 临时缓冲设为 64 B；生成的 TX 临时缓冲只用于初始化占位，实际发送数组由 `bsp_usb` 持有。不保留两份大的 TX 缓冲。

## 4. USB 与 UART 用户驱动的共同接口

```c
bool bsp_uart_write(const void *data, size_t size);
size_t bsp_uart_read(void *data, size_t size);

bool bsp_usb_write(const void *data, size_t size);
size_t bsp_usb_read(void *data, size_t size);
```

| 约定 | 两个驱动必须相同的行为 |
|---|---|
| 发送调用位置 | 正常运行时只有 `app_sample()` 这条路径生产 TX 数据 |
| `write()` 成功 | 整份数据已复制入驱动缓冲，可以立即复用调用方数组；不代表 PC 已处理 |
| `write()` 失败 | 整份数据未接收，不接收半帧；空间不足计队列拒绝，USB 未连接返回 false 但不计溢出 |
| 单次大小 | 保留现有 UART 的最多 256 B 限制，USB 使用同样的限制 |
| `read()` | 立即返回实际读取字节数；无数据返回 0，不保证读满 |
| RX 消费者 | 每个通道只有主循环的 `debug_poll()` 一个消费者 |
| 内容 | 驱动只处理字节，不理解 CR/LF、JustFloat、rpm 或电机状态 |
| 等待 | 不等待发送完成、不循环等待 BUSY；没有收发超时参数 |

保留必要的生命周期/中断入口：UART 的 `init/tick/irq/rx_irq`，USB 的 `poll` 和 CDC 回调接入。它们不是另一套业务收发 API。头文件用简短注释区分“字节收发”和“生命周期/IRQ 接入”，不引入额外私有头文件。

### UART 修改范围

- 保留已有双 256 B TX 缓冲、128 B RX DMA 缓冲和 1 Mbaud、8N1。
- 保留采样处理完成后 `bsp_uart_tick()` 启动 TX DMA 的时机；不移到 SysTick 或 USB 回调。
- 保留必要的发送拒绝、DMA、接收丢失与接收错误统计，不为统一接口重写已经工作的 DMA 实现。
- 当前 UART BSP 包含 `app.h/foc.h` 并直接调用 `app_fault(FOC_COMM)`。把这项电机策略移到 `debug/app` 层：BSP 只恢复 RX DMA 并报告明确的 RX DMA 故障事件。
- RX DMA 故障事件必须与 TX DMA 错误区分；可增加一个 RX DMA 故障计数。所选命令通道发生这类故障时保留现有 FOC_COMM 行为；普通发送队列满不触发它。

### USB 发送实现

- 第一版采用固定 8 KB TX 环形缓冲，放普通 SRAM，不新增 CCM 链接段。缓冲尺寸放 `bsp_usb.c` 内，不成为用户配置项。
- 单生产者复制完整调用数据后，以内存屏障发布写指针；消费者只能发送已经发布的范围。
- 主循环 `bsp_usb_poll()` 在空闲时提交当前连续区间，单次最多 256 B；环形回绕时分段提交。
- 不使用旧驱动的“积累 4 KB 或等待 10 ms”门槛。1 kHz 小帧在主循环有机会时立即提交。
- 提交成功只记录在途长度，不释放队列空间。ST 栈的完整发送完成回调到来后才推进读指针，包含必要的 ZLP。
- BUSY 保留待发数据，在后续主循环重试；完成回调只释放空间，不在回调里连续发大批数据。
- 队列满拒绝整次写入并计数，后续有空间继续接收遥测。不沿用旧版溢出后永久停止遥测的锁存策略。
- USB 未配置、DTR 未打开或挂起时不接收新遥测，也不将正常未连接状态计为队列溢出。
- 不直接发送应用的栈数组；不在 USB 发送路径全局关闭中断。

8 KB / 84,000 B/s 约为 97.5 ms 的名义容量，含在途数据；这只是选型计算，不是无损停读保证。此前 720 KB/s 长测和 50 ms 停读通过使用的是 64 KB 队列，不能直接作为 8 KB 的验收结论。

### USB 接收实现

- 沿用历史单包背压方法：CDC 收到 OUT 数据后保留其静态 64 B 缓冲，不立即重新挂接 OUT。
- `bsp_usb_read()` 从该包返回字节，全部消费后由 `bsp_usb_poll()` 重新挂接接收；期间 USB 自动 NAK，不覆盖未消费的数据。
- 接收消费与重新挂接不能被 TX BUSY、主机不读波形或队列满阻止。
- 无须再建立 RX 环形队列，也不在接收回调里执行命令。

主循环与 USB IRQ 共享状态时短暂屏蔽 USB IRQ，采样 IRQ 保持可抢占。TX 复制期间不屏蔽采样；屏蔽 USB IRQ也不等于保护了采样生产者，队列发布、释放和复位必须分别检查其所有权。

## 5. 生成回调与用户驱动的连接

| 生成位置/事件 | 用户代码职责 |
|---|---|
| `CDC_Init_FS` | 初始化会话收发状态，登记 RX 缓冲 |
| `CDC_DeInit_FS` | 在 ST 栈结束该类的传输后清理驱动状态 |
| `CDC_Receive_FS` | 调用 `bsp_usb_received()` 登记本包数据 |
| `CDC_TransmitCplt_FS` | 调用 `bsp_usb_transmitted()` 释放完成的在途数据 |
| `CDC_Control_FS` | 保存/返回 7 B line coding，处理 DTR 状态 |
| PCD Suspend/Resume | 暂停/恢复发送，报告命令会话中断 |
| USB Reset/Disconnect | 按 ST 栈实际回调顺序结束会话、重置缓冲 |

`CDC_Transmit_FS()` 是 USB 用户驱动调用的底层适配函数。业务应用只调用 `debug_write()`，不调用它。

DTR 关闭时停止生产新遥测、清理待执行 RX 数据，并通知主循环清除半条命令。正在发送的 TX 内存仍由 USB 持有，未提交队列在安全点丢弃。

重新打开前必须完成上次会话的安全清理；USB Reset/DeInit 后也要清除半条命令，不能只依赖 DTR 打开次数。使用一个会话代数/事件计数通知主循环即可，不增加通用事件系统。

不复制历史“FlushEP 后直接把 TxState 清零”的做法。底层确认传输结束/取消前不能复用在途内存；总线复位后不能重复释放旧长度。挂起也不能释放在途内存后立即复用。

PC 端同样按新会话重新同步并丢弃旧接收缓冲。设备无法撤回已经交给操作系统的字节，不承诺 DTR 关闭后主机绝对收不到旧尾部。

## 6. JustFloat 协议代码

只负责给已经填写好的 float 数组补帧尾，并返回字节数。通道选择由 `app.c` 决定，传输选择由 `debug.c` 决定，协议头不包含任何 UART/USB 头文件。

`App/Protocols/JustFloat/justfloat.h` 的核心实现：

```c
#pragma once
#include <math.h>
#include <stddef.h>

/* frame 至少有 count + 1 个 float，前 count 个已经填写。 */
static inline size_t justfloat_pack(float *frame, size_t count)
{
    frame[count] = INFINITY;
    return (count + 1u) * sizeof *frame;
}
```

应用沿用原来的数组，只改成显式的 21 个元素，并由协议函数填写最后一项：

```c
float frame[21] = { /* 原来的 20 个通道，原样保留顺序 */ };
/* 原来的状态字、故障诊断通道赋值保留。 */
debug_write(frame, justfloat_pack(frame, 20u));
```

这里省略号式注释只是接口说明，实施时使用现有真实通道表达式。没有中间打包数组，没有新增 memcpy，也不恢复旧宏的 16 通道限制或 UART 隐式时间戳。

固定使用 F405 的小端 IEEE754 float32；帧为 20 × 4 B 数据加 `00 00 80 7F`，共 84 B。不增加帧头、CRC、消息类型或另一套二进制命令协议。USB 可以在任意字节处分包，应用帧不必是 64 B 的整数倍。

主机按固定 84 B 帧长解析；失步时结合连续多个帧尾和通道有效性重新同步，不能仅搜索一次帧尾就判断成功。

## 7. 调试总配置与最小应用入口

`App/Debug/debug.h` 同时放配置和接口。上半部分是用户修改的配置，下半部分是函数声明：

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

#define DEBUG_USB_ENABLE   1
#define DEBUG_UART_ENABLE  0

#define DEBUG_CMD_NONE     0
#define DEBUG_CMD_USB      1
#define DEBUG_CMD_UART     2
#define DEBUG_CMD_SOURCE   DEBUG_CMD_USB

#define DEBUG_TELEMETRY_HZ 1000u

void debug_init(void);
void debug_write(const void *data, size_t size);
void debug_tick(void);
void debug_poll(void);
uint32_t debug_tx_rejected(void);
```

编译期检查命令来源是否合法、选定来源是否启用，以及回传频率能否整除当前 20 kHz 基准。只检查用户实际会修改的配置，不增加运行时检查。

| USB | UART | 命令源 | 行为 |
|---:|---:|---|---|
| 1 | 0 | USB | USB 回传及 USB 命令，默认配置 |
| 0 | 1 | UART | UART 回传及 UART 命令 |
| 1 | 1 | USB | 两路回传，只有 USB 命令执行 |
| 1 | 1 | UART | 两路回传，只有 UART 命令执行 |
| 0 | 0 | NONE | 不初始化这两个调试通道，控制正常运行 |

启用而未选为命令源的通道，收到数据也要消费并丢弃，避免 UART RX 溢出或 USB OUT 一直 NAK；不能更新命令计数、命令结果、命令 LED 和电机目标。

应用和生成文件的 USER CODE 都只需包含 `debug.h`。头文件不包含 HAL、USB 栈或 BSP 头；`debug.c` 再包含具体驱动，只定义函数和私有运行状态。配置宏只在头文件中定义一次。

- `debug_init()`：启动已启用通道的用户收发。USB 类状态由生成的 CDC 初始化回调建立；不额外重置已开始枚举的 USB 栈。
- `debug_write()`：向所有已启用通道入队，同一份完整帧分别复制。某一路拒收不阻止另一路；不对外返回一个含义不明的总成功值。
- `debug_tick()`：在每次采样的控制处理结束后调用，只推进已启用的 UART TX DMA，保留当前时序位置。
- `debug_poll()`：主循环 USB 服务、两路 RX 消费及唯一命令源组行。
- `debug_tx_rejected()`：读取所选命令通道的现有发送拒绝计数；命令源 NONE 返回 0。供既有通道 18 使用，不新增缓存变量或通道。另一通道统计仍可通过 BSP 计数观察。

`debug_tick()` 和 `debug_poll()` 分开是因为 UART 提交需要跟随采样，而 USB 栈服务及命令处理放主循环。统计入口是为保留既有遥测通道、避免 `app.c` 引用具体驱动；其余仅包装一行而无职责的函数不添加。

发送分发的主体就是两段编译条件，不建立通用 transport 层：

```c
void debug_write(const void *data, size_t size)
{
#if DEBUG_USB_ENABLE
    bsp_usb_write(data, size);
#endif
#if DEBUG_UART_ENABLE
    bsp_uart_write(data, size);
#endif
}
```

`debug_poll()` 复用当前 `app_poll()` 中的 64 字符行缓冲、长度和 overflow 逻辑，最多 63 个可见字符，处理 CR/LF/CRLF、拆包和粘包。每轮每个通道最多消费 64 B，使持续输入也不会无限占住后台处理。

接收错误/丢字节时，损坏的一行拒绝到下一个结束符；会话重建时清空半行，让新会话从新命令开始。主循环检查会话和错误事件后再解析。全部关闭和命令源 NONE 时不执行命令。

完整命令调用现有 `app_command()`。小数解析、电机状态条件、参数范围和 Flash 保存仍在 `app.c`，不建立第二份命令表。

## 8. app、main、生成初始化与构建接入

| 位置 | 实施修改 |
|---|---|
| `app_init()` | 将 `bsp_uart_init()` 替换为 `debug_init()`，放在编码器初始化前 |
| `telemetry()` | 保留 20 通道内容，调用 JustFloat 原地打包及 `debug_write()`；通道 18 改读 `debug_tx_rejected()` |
| `app_sample()` | 分频从 40 改为 `20000u / DEBUG_TELEMETRY_HZ`，当前为 20；末尾 `bsp_uart_tick()` 换为 `debug_tick()` |
| `app_poll()` | 原命令组行移入 `debug_poll()`；命令 LED 定时、保存与采样恢复逻辑留在原处 |
| `app_command()` | 保持现有命令解析与执行；不直接增加 USB/UART 来源分支 |
| `main.c` | 生成 USB 初始化调用；电机初始化失败时保留通信服务和中断，但不执行电机命令或进入正常采样恢复路径 |
| `usart.c` | USER CODE 早期按 DEBUG_UART_ENABLE 决定是否真正初始化 USART2 |
| `usb_device.c` | USER CODE PreTreatment 按 DEBUG_USB_ENABLE 决定是否启动 USB Device |
| 顶层 `CMakeLists.txt` | 加入两个新 `.c` 及 Debug/JustFloat include 路径；检查 Debug 下关键 USB FIFO 服务代码优化 |
| 生成 CMake | 由 CubeMX 更新，不手工编辑 |

禁用配置要覆盖硬件初始化、BSP 启动、收发和 IRQ 钩子接入，不能只停止发送但仍启动 CDC 枚举。生成源码可以仍参与构建，不增加删除库源文件的配置机制。共享的系统 DMA 初始化不整体关闭。

主循环及时消费命令，不等下一次 1 kHz 遥测。正常运行条件下，验证从 MCU 收到完整命令行到 `app_command()` 开始处理的延迟目标为不超过 2 ms；这是待测验收目标，不是已有实测值，也不是 PC 发出命令到接收到回传的总延迟。

`save` 和校准写 Flash 阶段保留已有停机/停采样流程，单独记录通信暂停；不承诺 Flash 擦写时的上述延迟或连续遥测。

## 9. 实施顺序与每步交付

1. **保存基线并生成 USB。** 记录干净/已有改动、关键源文件、链接脚本和原始配置；板上工作开始前备份参数及校准区。调整 `.ioc`、重新生成、对比差异，先让 Debug/Release 构建通过。
2. **完成 BSP。** 写 `bsp_usb.c/.h`，保留 UART DMA 实现，统一收发语义；接入完成、接收及会话回调，检查在途缓冲生命周期和 RX 背压。
3. **完成 JustFloat、debug 与 app 接入。** 添加小协议头及 `debug.c/.h`，将配置和接口一起放进 `debug.h`，使用同一帧进行通道分发；保持已有控制逻辑。
4. **检查配置组合与生成可重复性。** 构建表中的五组配置，再次 CubeMX 生成，确认 USER CODE 和关键外设配置不丢失；恢复默认 USB 配置。
5. **做临时主机检查。** 按本次用户要求直接运行内存中的检查，不创建本地测试源文件或测试框架；采集数据及结果记录放忽略的 `build/bench_debug/`。核对帧字节、整次入队、BUSY、环形回绕、延迟完成和会话复位。
6. **执行板上通信及控制验收。** 使用已核实设备和保留的 Flash 校准，按下一节执行，保存版本、配置、数据、计数和结果；先核实功率关闭，再分阶段烧录验证。
7. **更新当前使用文档。** `App/README.md` 更新 USB/UART 端口、DTR、命令来源、1 kHz、通道 18 含义、断连及 Flash 暂停行为；`AGENTS.md` 更新允许的调试传输与频率。

## 10. 验证项目与通过条件

构建命令在仓库根目录执行：

```powershell
cmake --preset Debug
cmake --build --preset Debug
cmake --preset Release
cmake --build --preset Release
git diff --check
```

| 验证 | 必须检查的结果 |
|---|---|
| 五种配置组合 | Debug/Release 都通过；所选命令源确实启用，非法组合编译拒绝 |
| CubeMX 再生成 | USER CODE 保留，构建通过，ADC/TIM/SPI/Flash 布局符合当前基线 |
| JustFloat 字节 | 20 个 float 顺序一致，末尾为 00 00 80 7F，共 84 B；UART/USB 字节一致 |
| TX 所有权 | 调用后立刻覆盖原数组不影响发送；延迟完成和 ZLP 之前不复用在途内存 |
| 队列满/BUSY | 拒绝完整消息、计数；不出现半帧入队，主机恢复后继续遥测 |
| RX 组行 | 单条拆多包、多条在一包、CR/LF/CRLF、空行、非法及超长命令符合现有语义 |
| 命令路由 | 两路分别发送命令，只有选定来源更新计数、LED、目标和结果；另一通道数据被消费 |
| 断连与重开 | 进行至少 10 次会话切换；半条命令不跨会话拼接，旧 TX 不覆盖、不重复释放 |
| 主机停读 | 50 ms 停读目标为本测试无缺帧；500 ms 作压力工况，允许明确丢整帧，随后必须恢复发送和命令处理 |
| TX 堵塞时命令 | 保持接收服务，完整命令仍能处理；验证 stop 不等待 TX 排空 |
| 长时间回传 | 1 kHz、84 KB/s，USB 单独和两路同时至少各 15 分钟；正常持续读取期间零内部丢帧、零格式错误、零队列拒绝 |
| 电机运行 | 覆盖已适合本机的速度/位置、正反转和不同母线工况；通信完整性与控制跟踪结果分别记录 |
| 采样时序 | 比较启用前后的 motor_work_max、motor_period_min/max、motor_write_min；无新采样/编码器/时序故障，CCR 提交继续满足现有 CNT >= 600 条件 |
| 初始化失败 | USB 仍可枚举及服务，电机功率保持关闭，正常启动命令不执行 |
| 保存参数 | 保留原 Flash 区及记录格式；保存前后数据回读一致，通信恢复后下一条命令正常 |

当前正式 20 通道帧没有遥测序号，所以不能只靠“收到了大约 1000 帧/s”宣布无丢帧。验收时临时在正常非故障帧的通道 19 写入 24 位连续遥测序号，按每帧加 1 校验；故障工况另行验证原诊断含义。插桩只用于临时验证，不成为正式协议或新的用户配置项。撤掉插桩后再构建并进行正式协议检查。

会话切换、采样重启和 Flash 擦写单独划分记录，不把设计上的暂停混入持续采集窗口。OS 缓冲可能使应用暂停读取尚未立刻造成设备溢出，停读实验按实际序号和 MCU 队列结果判定，不把理论容量当实测结果。

至少记录固件版本、配置、帧长/帧率、实际捕获窗口、序号缺口、发送拒绝、队列峰值、RX 错误及电机时序。新 8 KB 驱动未经这些检查不能借用旧 64 KB 驱动的“无丢帧”结论。

## 11. 历史证据与本计划的性能依据

| 历史结果 | 对本计划的意义 |
|---|---|
| 720 KB/s，900 s，18,000,212 帧无缺口，电机功率关闭 | 证明本机旧 USB 方案有高于 84 KB/s 的吞吐基础 |
| 720 KB/s 上行 + 16.384 KB/s 下行，60 s 通过 | 证明旧方案有双向通信基础，不代表新实现下行上限 |
| 1040 KB/s 曾有 180 s 通过，也有后续溢出反例 | 第一版不按该带宽设计或承诺 |
| 128 KB/s，12/18/24 V、39 工况、1,048,597 帧零丢帧 | 有带电机实测支持 1 kHz、84 KB/s 的目标量级 |

历史结果见 [USB_TEST.md](https://github.com/ZhangStudyLife/F405_FOC/blob/18baaee/tests/USB_TEST.md)、[9/26 报告](https://github.com/ZhangStudyLife/F405_FOC/blob/405b01e/build/bench_debug/20260926_idle_fix/report.md)、[9/26 统计 JSON](https://github.com/ZhangStudyLife/F405_FOC/blob/405b01e/build/bench_debug/20260926_idle_fix/summary.json)。本机未找到高带宽测试的原始二进制文件，不能声称已经重新逐字节复核那些记录。

当前 UART 1 Mbaud、8N1 的有效线速上限是 100,000 B/s；84 KB/s 占 84%，单帧在线约 840 us，因此双路 1 kHz 时 UART 仍须实测。USB 的 64 B 最大数据包见 [ST CDC 头文件](https://raw.githubusercontent.com/STMicroelectronics/stm32-mw-usb-device/master/Class/CDC/Inc/usbd_cdc.h)；指针持有、发送状态及完成回调行为见 [ST CDC 实现](https://raw.githubusercontent.com/STMicroelectronics/stm32-mw-usb-device/master/Class/CDC/Src/usbd_cdc.c)。实施使用工程匹配的库版本，这些官方源码只用于核对机制。

## 12. 简洁性检查与完成标准

- 新增文件限于这套目录中的 USB BSP、`debug.c/.h` 和 JustFloat 头；配置合并进 `debug.h`，不建立另一套通用通信框架。
- 配置不增加 DMA 模式、缓冲大小、超时、传输批量、命令表或回调注册等用户没有要求的开关。
- 只为实际状态保留变量；不缓存能直接读取的计数，不新增无意义的长度别名或中间数组。
- 只处理会实际发生的队列满、BUSY、RX 损坏和会话中断，不设计通用异常分类或自动重启整机策略。
- 头文件的每个声明都必须有当前调用点或必要的生成回调接入；仅转调一次且没有职责的包装函数删除。
- 简单语句直接写；需要队列所有权、会话清理和中断协作的地方优先保证正确，不为了压缩行数隐藏状态或改变时序。
- 完成意味着所有配置、生成再现、协议、路由、恢复和板上检查有对应证据；只有编译通过时不得宣布 1 kHz 实机无丢帧。

最终交付为：CubeMX 可再生成的 USB 集成、参数/返回值一致的 UART/USB 字节接口、一个小 JustFloat 头、唯一的 debug 配置、1 kHz 同帧回传与唯一命令入口，以及本机可复核的验收记录。
