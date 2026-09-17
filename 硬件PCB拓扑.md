# 1. 整块板子的硬件拓扑

四页原理图合并后的系统结构：

```text
                         ┌─────────────────────────────┐
                         │        DCBUS 直流母线        │
                         │   J9 + 大容量母线电容组       │
                         └──────────────┬──────────────┘
                                        │
             ┌──────────────────────────┼──────────────────────────┐
             │                          │                          │
             │                          │                          │
             ▼                          ▼                          ▼
       Motor 0 功率级              Motor 1 功率级              AUX 半桥
       6×NMOS Q1~Q6              6×NMOS Q9~Q14              Q7 + Q8
             │                          │                          │
        A/B/C 三相                  A/B/C 三相                 AUX_OUT
             │                          │                          │
            J14                        J19                        J23
             ▲                          ▲                          ▲
             │                          │                          │
          EG2134 U5                 EG2134 U6                 EG2132 U12
             ▲                          ▲                          ▲
        6路 PWM                     6路 PWM                     2路 PWM
             └──────────────┬───────────┴──────────────┬───────────┘
                            │                          │
                         STM32F405 U2                TIM输出
                            │
             ┌──────────────┼───────────────────────────────┐
             │              │               │               │
             ▼              ▼               ▼               ▼
          电流ADC          编码器           CAN             USB
       M0/M1 SO1/SO2      A/B/Z         TJA1051T/3        Type-C
             ▲
             │
      1mΩ Shunt + 运放
      1.65V 中点偏置


DCBUS
  │
  ├── 电阻分压 → VBUS_S → STM32 ADC
  │
  └── XL7005A
          │
        "12V"
          │
          ├────────→ EG2134 / EG2132 栅极驱动
          │
          └── RY8411
                │
               5V
                │
                └── AMS1117
                       │
                      VCC
                    ≈ 3.3V
```

## 板外接线

| 外部设备              | 接入位置                    | 板上网络 / MCU 引脚 | 详见   |
| --------------------- | --------------------------- | ------------------- | ------ |
| 上位机 PC（调试数据） | USB 转 TTL 模块，接 J3 排针 | PA2 / PA3（USART2） | §19   |
| 三相电机              | J19（Motor1）               | `M1_A/B/C`        | §20   |
| MT6835 磁编码器       | J3 排针                     | SPI3 + PA0 片选     | §11.1 |
| ST-Link 调试器        | J2                          | SWDIO / SWCLK       | §17   |
| CAN 总线              | J3 pin3 / pin4              | PB8 / PB9           | §12   |

---

# 2. 电源树：DCBUS → 12V → 5V → VCC

## DCBUS 母线

外部直流输入接入 `DCBUS / PGND`。

母线上直接并接：

- C26～C33：8 × 100 µF
- C38～C42：100 nF / 100 V
- Motor0 三相桥
- Motor1 三相桥
- AUX 半桥
- 辅助电源 U7

```text
电源输入
   │
DCBUS ─────────────────────────────────────────────
   │          │               │               │
 大电容      M0功率桥          M1功率桥         AUX功率桥
   │
   └────→ 辅助电源
PGND ─────────────────────────────────────────────
```

DCBUS 直接作为 Motor0 / Motor1 / AUX 三相桥 MOSFET 高侧的功率母线。

---

## 第一级：DCBUS → 12V

U7 = **XL7005A**，异步 Buck 拓扑。

```text
DCBUS
  │
  ▼
XL7005A
  │ SW
  ├──── L1 = 100 µH ────→ 12V
  │
 D1 = S210
  │
 GND
```

反馈：

```text
12V ── R8 20k ──┐
                 ├── FB
GND ── R7 2.2k ─┘
```

`V_OUT = 1.25 V × (1 + R8/R7)`，按 20 kΩ / 2.2 kΩ 计算约 **12.6 V**。
原理图网络名为 **12V**。

12V 的去向：

```text
12V
 ├── U5 EG2134 → Motor0 MOS Gate
 ├── U6 EG2134 → Motor1 MOS Gate
 ├── U12 EG2132 → AUX MOS Gate
 └── U3 → 5V
```

---

## 第二级：12V → 5V

U3 = **RY8411**，功率电感 L2 = 4.7 µH。

```text
12V
 │
 ▼
RY8411
 │ SW
 ▼
L2 4.7µH
 │
 ▼
5V
```

反馈电阻 R13 = 51 kΩ、R14 = 10 kΩ。

---

## 第三级：5V → VCC

U4 = **AMS1117**。

```text
5V → AMS1117 → VCC
```

网络名为 `VCC`（原理图未标注 `3V3`）；该网络同时连接 STM32F405 的 I/O 供电、
编码器上拉电阻、TJA1051T/3 的 VIO，取值 **≈ 3.3 V**。

完整电源树：

```text
DCBUS
  │
  └── XL7005A
        ↓
      ≈12.6V ("12V")
        │
        ├── Gate Drivers
        │
        └── RY8411
              ↓
             5V
              │
              └── AMS1117
                    ↓
                   VCC ≈ 3.3V
```

---

# 3. STM32F405 管脚分配

原理图 U2 = STM32F405。

PWM 网络：

| 功能       | MCU              | 网络      |
| ---------- | ---------------- | --------- |
| M0 A相高侧 | PA8 / TIM1_CH1   | `M0_AH` |
| M0 B相高侧 | PA9 / TIM1_CH2   | `M0_BH` |
| M0 C相高侧 | PA10 / TIM1_CH3  | `M0_CH` |
| M0 A相低侧 | PB13 / TIM1_CH1N | `M0_AL` |
| M0 B相低侧 | PB14 / TIM1_CH2N | `M0_BL` |
| M0 C相低侧 | PB15 / TIM1_CH3N | `M0_CL` |
| M1 A相高侧 | PC6 / TIM8_CH1   | `M1_AH` |
| M1 B相高侧 | PC7 / TIM8_CH2   | `M1_BH` |
| M1 C相高侧 | PC8 / TIM8_CH3   | `M1_CH` |
| M1 A相低侧 | PA7 / TIM8_CH1N  | `M1_AL` |
| M1 B相低侧 | PB0 / TIM8_CH2N  | `M1_BL` |
| M1 C相低侧 | PB1 / TIM8_CH3N  | `M1_CL` |
| AUX Low    | PB10 / TIM2_CH3  | `AUX_L` |
| AUX High   | PB11 / TIM2_CH4  | `AUX_H` |

定时器分组：

```text
TIM1
 └── Motor0：3组 CHx + CHxN → 三相互补 PWM

TIM8
 └── Motor1：3组 CHx + CHxN → 三相互补 PWM

TIM2
 └── CH3 + CH4 → AUX 半桥
```

---

# 4. Motor 0：三相桥拓扑

Motor0 在原理图第 2 页。

Gate Driver：**U5 = EG2134**

输入：

```text
STM32                       U5 EG2134
M0_AH ───────────────────→ HIN1
M0_BH ───────────────────→ HIN2
M0_CH ───────────────────→ HIN3

M0_AL ───────────────────→ LIN1
M0_BL ───────────────────→ LIN2
M0_CL ───────────────────→ LIN3
```

U5 引脚包括 3 路 HIN/LIN、3 路 HO/LO、VB/VS。

功率级：

```text
           DCBUS
             │
       ┌─────┼─────┐
       │     │     │
      Q1    Q2    Q3
       │     │     │
       A     B     C ─────→ J14 → Motor0
       │     │     │
      Q4    Q5    Q6
       │     │     │
       └─────┴─────┘
             │
            PGND
```

桥臂组成：

```text
A相：Q1 High + Q4 Low
B相：Q2 High + Q5 Low
C相：Q3 High + Q6 Low
```

栅极驱动路径，每路串 22 Ω（R29～R34）：

```text
HO1 → M0GH_A → 22R → Q1 Gate
LO1 → M0GL_A → 22R → Q4 Gate

HO2 → M0GH_B → 22R → Q2 Gate
LO2 → M0GL_B → 22R → Q5 Gate

HO3 → M0GH_C → 22R → Q3 Gate
LO3 → M0GL_C → 22R → Q6 Gate
```

---

# 5. Bootstrap 网络

EG2134 高侧驱动采用自举（Bootstrap）供电。

A 相：

```text
12V
 │
 D2
 │
 ├──── VB1
 │      │
 │     C46 10µF
 │      │
 └──── VS1 = M0SH_A
```

B / C 相分别为 D3/C47、D4/C48。

```text
12V → Bootstrap Diode → VBx
                          │
                    Bootstrap Cap
                          │
                       VSx/相点
```

Motor1 为同样的网络结构。

---

# 6. 电流采样：两低侧 Shunt

Motor0 分流电阻：

```text
R35 = 0.001Ω
R36 = 0.001Ω
```

即 **1 mΩ**，接在低侧 MOSFET 源极与 PGND 之间：

```text
A相 Low Q4 ───────────────→ PGND
                           无 Shunt

B相 Low Q5
     │
 M0_SN1
     │
 R35 = 1mΩ
     │
 M0_SP1
     │
    PGND


C相 Low Q6
     │
 M0_SN2
     │
 R36 = 1mΩ
     │
 M0_SP2
     │
    PGND
```

A 相低侧无分流电阻，只有 B、C 两相安装 Shunt。

Motor1 同构：R45、R46，各 1 mΩ。

---

# 7. 电流采样模拟前端

参考电压由 U13 产生：

```text
VCC
 │
R50 10k
 │
 ├──→ U13 Buffer → 1V65
 │
R63 10k
 │
GND
```

`V_REF = VCC/2 ≈ 1.65 V`。

以 M0_SO1 为例：

```text
M0_SP1 ── R55 1k ──→ U14A (+)
                       ↑
                 R51 20k
                       │
                     1V65


M0_SN1 ── R64 1k ──→ U14A (-)
                       │
                R65 20k feedback
                       │
                    Output
                       │
                    R59 100R
                       │
                    M0_SO1 → MCU ADC
                       │
                    C70 2.2nF
                       │
                      GND
```

电阻取值与结果参数：

| 项目               | 值                                       |
| ------------------ | ---------------------------------------- |
| 输入电阻 R55 / R64 | 1 kΩ                                    |
| 反馈电阻 R51 / R65 | 20 kΩ                                   |
| 差分增益           | 20（20 kΩ / 1 kΩ）                     |
| 传输关系           | `V_SO = 1.65 V + 20 × (V_SP − V_SN)` |
| 分流电阻 R_S       | 1 mΩ                                    |
| 电流灵敏度         | 20 mV/A（1 A ≈ 20 mV）                  |
| 输出串阻 R59       | 100 Ω                                   |
| 输出滤波电容 C70   | 2.2 nF                                   |

网络命名方向：低侧正向电流时 `V_SN > V_SP`，输出相对 1.65 V 中点向下变化。

---

# 8. 电机电流 ADC 映射

```text
Motor0
M0_SO1 → PC0 / ADC10
M0_SO2 → PC1 / ADC11

Motor1
M1_SO2 → PC2 / ADC12
M1_SO1 → PC3 / ADC13
```

四路 ADC 输入为 PC0、PC1、PC2、PC3 连续排列。

Motor1 功率级与 Motor0 结构相同：U6 EG2134、Q9～Q14。

| 网络       | MCU 引脚    |
| ---------- | ----------- |
| `M0_SO1` | PC0 / ADC10 |
| `M0_SO2` | PC1 / ADC11 |
| `M1_SO2` | PC2 / ADC12 |
| `M1_SO1` | PC3 / ADC13 |

---

# 9. 电机温度采样

Motor0：

```text
VCC
 │
NTC 10k R27
 │
 ├──── M0_TEMP → MCU PC5/ADC15
 │
 ├──── C45 2.2µF → GND
 │
R28 3.3k
 │
GND
```

Motor1：

```text
NTC R37 → M1_TEMP → PA4/ADC4
```

AUX：

```text
NTC R48 → AUX_TEMP → PA5/ADC5
```

| 网络         | MCU 引脚    |
| ------------ | ----------- |
| `M0_TEMP`  | PC5 / ADC15 |
| `M1_TEMP`  | PA4 / ADC4  |
| `AUX_TEMP` | PA5 / ADC5  |

---

# 10. DCBUS 母线电压检测

DCBUS 电阻分压：

```text
DCBUS
 │
R5 = 39k
 │
 ├──── VBUS_S ───→ PA6 / ADC6
 │        │
 │      C3 100nF
 │        │
R12=2.2k │
 │        │
 └────────┴── GND
```

| 项目                    | 值                                                     |
| ----------------------- | ------------------------------------------------------ |
| 上分压电阻 R5           | 39 kΩ                                                 |
| 下分压电阻 R12          | 2.2 kΩ                                                |
| 滤波电容 C3             | 100 nF                                                 |
| 分压比                  | `V_BUS_S = V_BUS × 2.2/(39+2.2) ≈ 0.0534 × V_BUS` |
| ADC 引脚                | PA6 / ADC6                                             |
| 满量程（按 3.3 V 参考） | ≈ 61.8 V                                              |

---

# 11. 编码器网络（J4，A/B/Z）

Motor0：

```text
M0_ENC_A → PB4 / TIM3_CH1
M0_ENC_B → PB5 / TIM3_CH2
M0_ENC_Z → PC9
```

Motor1：

```text
M1_ENC_A → PB6 / TIM4_CH1
M1_ENC_B → PB7 / TIM4_CH2
M1_ENC_Z → PC15
```

A/B/Z 三路信号各有 **3.3 kΩ 上拉到 VCC**。

编码器接插件 J4：

```text
J4

Pin 1  VCC
Pin 2  5V
Pin 3  M1_ENC_A
Pin 4  M1_ENC_B
Pin 5  M1_ENC_Z
Pin 6  GND

Pin 7  VCC
Pin 8  5V
Pin 9  M0_ENC_A
Pin10  M0_ENC_B
Pin11  M0_ENC_Z
Pin12  GND
```

J4 同时提供 **5V** 与 **VCC** 两种供电。

---

## 11.1 MT6835 磁编码器：型号与引脚连接

本工程实际使用的编码器：

> **MagnTek（麦歌恩）MT6835GT-STD**
> TSSOP-16 封装，21 位绝对角度磁编码器，角度通过 4 线 SPI 读出。
> 感测元件阵列位于封装几何中心。

### 器件参数（数据手册 Rev 1.3）

| 项目         | 值                                                      |
| ------------ | ------------------------------------------------------- |
| 型号 / 封装  | MT6835GT-STD / TSSOP-16                                 |
| 角度输出     | 21 bit 绝对角度（`ANGLE[20:0]` 对应 0~360°）         |
| 主机接口     | 4 线 SPI（另有 ABZ / UVW / PWM 输出脚）                 |
| 供电 VDD     | 3.0 ~ 5.5 V                                             |
| 磁感应范围   | 30 ~ 1000 mT                                            |
| 转速上限     | 120,000 RPM                                             |
| SPI 时钟上限 | 16 MHz（TSCK ≥ 60 ns）                                 |
| 工作温度     | −40 ~ 125 °C                                          |
| 状态位       | bit0 超速 / bit1 磁场过弱 / bit2 欠压（随角度一起读出） |

### 芯片引脚定义（数据手册 1 节、7.1 节）

| 脚号 | 名称    | 类型     | 功能                    | 本设计连接     |
| ---- | ------- | -------- | ----------------------- | -------------- |
| 1    | U       | 数字输出 | 换相 U 或差分 −A       | 悬空           |
| 2    | V       | 数字输出 | 换相 V 或差分 −B       | 悬空           |
| 3    | W       | 数字输出 | 换相 W 或差分 −Z       | 悬空           |
| 4    | CAL_EN  | 数字输入 | 用户自校准使能          | 悬空           |
| 5    | MISO    | 数字输出 | SPI 数据（芯片 → MCU） | PC11           |
| 6    | MOSI    | 数字输入 | SPI 数据（MCU → 芯片） | PC12           |
| 7    | SCK     | 数字输入 | SPI 时钟                | PC10           |
| 8    | CSN     | 数字输入 | SPI 片选（低有效）      | PA0            |
| 9    | VDD     | 电源     | 3.0 ~ 5.5 V 供电        | VCC（≈3.3 V） |
| 10   | OUT     | 数字输出 | PWM 角度输出            | 悬空           |
| 11   | TEST    | 模拟输入 | 工厂测试脚              | 悬空           |
| 12   | VSS     | 电源     | 地                      | GND            |
| 13   | TEST_EN | 数字输入 | 工厂测试使能            | 悬空           |
| 14   | Z       | 数字输出 | 增量 Z                  | 悬空           |
| 15   | B       | 数字输出 | 增量 B                  | 悬空           |
| 16   | A       | 数字输出 | 增量 A                  | 悬空           |

### 接线：MT6835 ↔ STM32

| MT6835 脚 | 信号           | 板级网络                    | MCU 引脚         | J3 脚               |
| --------- | -------------- | --------------------------- | ---------------- | ------------------- |
| 9 VDD     | 供电           | VCC（≈3.3 V）              | —               | 1 或 6              |
| 12 VSS    | 地             | GND                         | —               | 2 / 5 / 7 / 19 / 20 |
| 7 SCK     | SPI 时钟       | `SPI_SCK`                 | PC10 / SPI3_SCK  | 8                   |
| 5 MISO    | 角度数据回读   | `SPI_MISO`                | PC11 / SPI3_MISO | 9                   |
| 6 MOSI    | 命令下发       | `SPI_MOSI`                | PC12 / SPI3_MOSI | 10                  |
| 8 CSN     | 片选（低有效） | `GPIO_1` → `MT6835_CS` | PA0              | 11                  |

```text
   STM32F405                              J3 排针            MT6835
   ────────────────────────────────────────────────────────────────
   PC10 SPI3_SCK  ──────────────────────── pin8  ────────  7 SCK
   PC11 SPI3_MISO ──────────────────────── pin9  ────────  5 MISO
   PC12 SPI3_MOSI ──────────────────────── pin10 ────────  6 MOSI
   PA0  MT6835_CS ──────────────────────── pin11 ────────  8 CSN
   VCC  ≈3.3 V    ──────────────────────── pin1  ────────  9 VDD
   GND            ──────────────────────── pin2  ──────── 12 VSS
```

引脚按同名对接：MCU 的 MISO 接芯片 MISO，MOSI 接 MOSI（芯片引脚名按主机视角命名）。

### SPI 通信参数

| 项目        | 值                                                |
| ----------- | ------------------------------------------------- |
| 模式        | SPI 模式 3（CPOL=1，CPHA=1）                      |
| 位序 / 位宽 | MSB first，8 bit                                  |
| 时钟        | 上限 16 MHz；本工程 10.5 MHz（PCLK1 42 MHz ÷ 4） |
| 空闲电平    | 非通信时 SCK 保持高                               |
| 片选        | CSN 低有效，下降沿开始一帧、上升沿结束            |
| 读角度帧    | 6 字节突发读（命令`0xA0 0x03` + 4 字节）        |
| MISO 空闲态 | 高阻 Hi-Z                                         |

### 磁钢与安装要求（数据手册 6 节、11 节）

| 项目          | 要求                              |
| ------------- | --------------------------------- |
| 磁钢形式      | 圆柱形，径向（diametrically）充磁 |
| 磁钢厚度 Tmag | ≤ 2.5 mm                         |
| 磁钢中心轴    | 与芯片感测元件中心对齐            |
| 气隙 AG       | 尽可能小                          |
| 磁感应强度    | 30 ~ 1000 mT                      |

---

# 12. CAN 网络

```text
STM32 PB9 / CAN1_TX
        │
      CAN_D
        │
        ▼
     U1 TXD
   TJA1051T/3
     CANH/CANL
        │
        ▼
      CAN总线


CAN总线
  │
U1 RXD
  │
CAN_R
  │
STM32 PB8 / CAN1_RX
```

TJA1051T/3 供电：

```text
VCC → 5V
VIO → VCC（约3.3V）
```

终端电阻：

```text
CAN_H
 │
R3 120Ω
 │
SW1
 │
CAN_L
```

120 Ω 终端电阻经 SW1 接入 / 断开。

---

# 13. USB Type-C

```text
Type-C J1

CC1 ── 5.1k → GND
CC2 ── 5.1k → GND
```

数据线：

```text
Type-C D+
    │
   22Ω R6
    │
 USB_DP
    │
 STM32 PA12

Type-C D-
    │
   22Ω R1
    │
 USB_DM
    │
 STM32 PA11
```

USB VBUS 未接入板内电源树；板内网络 `VBUS_S` 是 DCBUS 的分压采样，与 USB VBUS 无关。

---

# 14. SPI / GPIO 扩展 J3

J3 为 20 Pin 扩展口：

```text
 1  VCC
 2  GND
 3  CAN_H
 4  CAN_L
 5  GND
 6  VCC
 7  GND
 8  SPI_SCK
 9  SPI_MISO
10  SPI_MOSI
11  GPIO_1
12  GPIO_2
13  GPIO_3
14  GPIO_4
15  GPIO_5
16  GPIO_6
17  GPIO_7
18  GPIO_8
19  GND
20  GND
```

SPI 网络：

```text
PC10 → SPI3_SCK
PC11 → SPI3_MISO
PC12 → SPI3_MOSI
```

J3 上被本工程使用的引脚：

- **pin 8 / 9 / 10 = SPI3 → MT6835 磁编码器**（见 §11.1）
- **pin 11 = GPIO_1 → PA0 = MT6835 片选**
- **pin 13 / 14 = GPIO_3 / GPIO_4 = PA2 / PA3 → USB 转 TTL 调试串口**（见 §19）
- **pin 3 / 4 = CAN_H / CAN_L**

---

# 15. GPIO 网络

直接连接的 GPIO：

```text
GPIO3 → PA2
GPIO4 → PA3
GPIO5 → PC4
```

经可配置焊桥网络的 GPIO 为 GPIO1、GPIO2、GPIO6、GPIO7、GPIO8。
以 GPIO1 为例：

```text
                 SB1
GPIO1 ─────┬────o/o────┬── GPIO1_FLT ─ SB2 ──┬── MCU PA0
           │            │                     │
           └─ R16 3.3k ─┘                   C16
                                              │
                                            82pF
                                              │
                                             GND
```

该网络可配置为：直连（SB1 短接）、经 3.3 kΩ（R16）、是否挂 82 pF（C16）。

GPIO2 / GPIO6 / GPIO7 / GPIO8 为同样结构，网络名为 `GPIO_2_FLT`、`GPIO_6_FLT` 等。

---

# 16. AUX 半桥

原理图第 4 页为 DCBUS 高压半桥：

```text
                 DCBUS
                   │
                  Q7
                   │
                   ├────────→ AUX OUT → J23 Pin2
                   │
                  Q8
                   │
                  PGND
                              J23 Pin1 → PGND
```

驱动器：

```text
U12 EG2132

AUX_H → HIN
AUX_L → LIN

HO → R47 22Ω → Q7
LO → R72 22Ω → Q8
```

Bootstrap：

```text
12V → D8 → Vbb
              │
            C63
              │
             VS
              │
          AUX switch node
```

AUX 输出以 DCBUS 为母线电压。J23 的外部负载原理图未标注。

---

# 17. SWD / 时钟 / Boot

外部晶振：

```text
Y1 = 8 MHz
C19 = 22 pF
C20 = 22 pF
```

SWD 接插件 J2：

```text
1 VCC
2 5V
3 SWCLK
4 SWDIO
5 nRST
6 GND
```

SWCLK / SWDIO 各串 22 Ω。

BOOT0：

```text
VCC
 │
R4 10k
 │
BOOT0
 │
SW1
 │
GND
```

SW1 为双路开关：一路接 BOOT0 到 GND，另一路接 CAN 120 Ω 终端。

---

# 18. 整板信号闭环

```text
                    STM32F405
                        │
                    TIM1 / TIM8
                        │
                6路互补PWM/电机
                        │
                        ▼
                    EG2134
                        │
                   Gate Drive
                        │
                        ▼
                  6 MOS 三相桥
                        │
                        ▼
                       电机
                        │
            ┌───────────┴───────────┐
            │                       │
        编码器 A/B/Z            相电流
            │                       │
      TIM3 / TIM4             1mΩ Shunt
            │                       │
            │                差分放大 Gain≈20
            │                       │
            │                   Mx_SOx
            │                       │
            └──────────────→ STM32 ADC
                                │
                                ▼
                          Clarke/Park
                                │
                           PI 电流环
                                │
                              SVPWM
                                │
                          回到 TIM PWM
```

信号链：

**PWM → Gate Driver → MOS Bridge → Motor → Current Sense → ADC / Encoder → MCU**

---

# 19. 上位机串口：PC 的 USB 转 TTL → PA2 / PA3

## 19.1 引脚

PA2 / PA3 引到 J3 扩展口：

```text
J3 pin 13   GPIO_3 ──→ PA2 ──→ USART2_TX（AF7）
J3 pin 14   GPIO_4 ──→ PA3 ──→ USART2_RX
J3 pin 2 / 5 / 7 / 19 / 20 ──→ GND
```

## 19.2 接线

| USB 转 TTL 模块 | 板子（J3）                | 说明                    |
| --------------- | ------------------------- | ----------------------- |
| TXD             | pin 13（PA2 = USART2_TX） | 模块 TXD 接板子的 TX 脚 |
| RXD             | pin 14（PA3 = USART2_RX） | 模块 RXD 接板子的 RX 脚 |
| GND             | pin 2（任一个 GND 脚）    | 共地                    |
| 电平            | —                        | 3.3 V                   |

```text
   ┌──────────────┐                      ┌──────────────────────┐
   │  PC USB 口    │                      │      STM32F405       │
   └──────┬───────┘                      │                      │
          │                              │                      │
   ┌──────┴───────┐   TXD ── J3 pin13 ──┤ PA2 / USART2_TX      │
   │ USB 转 TTL   │   RXD ── J3 pin14 ──┤ PA3 / USART2_RX      │
   │ （3.3 V 电平）│   GND ── J3 pin2 ──┤ GND                  │
   └──────────────┘                      └──────────────────────┘
```

## 19.3 串口参数

| 项目       | 值                                                              |
| ---------- | --------------------------------------------------------------- |
| 外设       | USART2，异步模式                                                |
| 波特率     | 460800                                                          |
| 数据格式   | 8 数据位 / 1 停止位 / 无校验 / 无硬件流控 / 16 倍过采样         |
| TX 引脚    | PA2，AF7                                                        |
| RX 引脚    | PA3                                                             |
| TX DMA     | DMA1 Stream6 / Channel 4，Normal、字节宽、内存递增、High 优先级 |
| 中断优先级 | DMA1_Stream6 = 5，USART2 = 6                                    |

---

# 20. 电机：5010-KV360

## 20.1 参数

| 参数        | 值         |
| ----------- | ---------- |
| 电机型号    | 5010-KV360 |
| 转速常数 Kv | 360 RPM/V  |
| 极对数      | 7（14 极） |
| 额定电压    | 24 V       |
| 最大转速    | 8600 RPM   |
| 最大电流    | 23 A       |
| 相电阻      | 0.12 Ω    |
| 相电感      | 50 µH     |
| 磁链        | 0.0021 Wb  |
| 扭矩        | 0.25 N·m  |
| 驱动线长度  | 300 mm     |
| 重量        | 90 g       |

## 20.2 接入点

电机接在板子的 **M1**（Motor1）功率级上：

| 项目     | 连接                                                 |
| -------- | ---------------------------------------------------- |
| 三相输出 | J19                                                  |
| 功率级   | Q9~Q14 三相桥，驱动 U6 EG2134                        |
| 分流电阻 | R45、R46 各 1 mΩ（M1 的 B / C 相低侧）              |
| 高侧 PWM | PC6 / PC7 / PC8（TIM8_CH1 / CH2 / CH3）              |
| 低侧 PWM | PA7 / PB0 / PB1（TIM8_CH1N / CH2N / CH3N）           |
| 电流采样 | PC3（`M1_SO1` / ADC13）、PC2（`M1_SO2` / ADC12） |
| 温度采样 | PA4（`M1_TEMP` / ADC4，NTC R37）                   |
| 编码器   | J3（MT6835，SPI3 + PA0 片选，见 §11.1）             |

板上另一路 Motor0（J14 / Q1~Q6 / U5 EG2134）未接电机：

| 项目     | 连接                                                     |
| -------- | -------------------------------------------------------- |
| 三相输出 | J14                                                      |
| 高侧 PWM | PA8 / PA9 / PA10（TIM1_CH1 / CH2 / CH3）                 |
| 低侧 PWM | PB13 / PB14 / PB15（TIM1_CH1N / CH2N / CH3N）            |
| 电流采样 | PC0（`M0_SO1` / ADC10）、PC1（`M0_SO2` / ADC11）     |
| 温度采样 | PC5（`M0_TEMP` / ADC15，NTC 10 kΩ R27 / R28 3.3 kΩ） |
