# tools：电机测试数据系统

本目录保存**主机端工具**。固件接口与命令见 [App/README.md](../../App/README.md)，
主机回归测试见 [tests/README.md](../../tests/README.md)，硬件接线见
[硬件PCB拓扑.md](../../硬件PCB拓扑.md)。

## bench/：一键电机台架

从仓库根目录用 `download/bench.cmd`，或直接调用 Python：

```sh
download\bench.cmd list                 # 列出串口，FOC 板前有 * 标记
download\bench.cmd discover             # 探活：帧率、字节率、帧尾校验、连续性
download\bench.cmd run --dry-run        # 只打印工况清单与预估时长，不发命令
download\bench.cmd run                  # 全量工况（需 --yes 或交互确认）
download\bench.cmd run --mode speed --groups 3 --per-cel 5
download\bench.cmd report %TEMP%\foc_bench\<时间戳>
download\bench.cmd chain  %TEMP%\foc_bench\<时间戳> step_pos_360
```

`run` 默认写入 `%TEMP%\foc_bench\<时间戳>\`（可用 `--out` 改）。每个
`(工况, 日志组)` 一个压缩包：`<工况>_g<组>_r<重复>.zip`，内含

| 文件 | 内容 |
|---|---|
| `meta.json` | 工况名、模式、组号、目标值、命令时间线、抖动、限制、git 版本、列名 |
| `frames.f32` | 原始帧流，**每帧 12 个小端 float32 = 48 字节，无帧尾**（帧尾在归档前已校验并剥离） |

`frames.f32` 可直接用 numpy 读：

```python
import numpy as np, json, zipfile
with zipfile.ZipFile("step_pos_360_g3_r0.zip") as z:
    meta = json.loads(z.read("meta.json"))
    data = np.frombuffer(z.read("frames.f32"), dtype="<f4").reshape(-1, 12)
```

或让库代劳（解压到临时目录并给出连续性统计）：

```python
import sys; sys.path.insert(0, r"E:\405_FOC\405_FOC\tools\bench")
import benchlib
table, meta = benchlib.load(r"...\step_pos_360_g3_r0.zip")
print(meta["continuity"])
```

## 高速帧格式（USB 20 kHz）

`send X` 之后每个 20 kHz 采样发一帧：**12 个小端 float32 + 帧尾 `00 00 80 7F` = 52 字节**。

| 下标 | 内容 |
|---|---|
| 0 | 低 24 位 = `t_u24`（TIM5 微秒，16.78 s 回绕）；高 8 位 = 状态字：bit0..2 状态、bit3..6 故障、bit7..8 功率模式 |
| 1 | 低 24 位 = `seq`（20 kHz 单调计数，13.98 min 回绕）；高 8 位 = 日志组号 |
| 2..11 | 该组的 10 个数据通道（见下表） |

**时间与丢帧**：`t_u24` 与 `seq` 是权威采样身份，与组号无关；组号切换不重置它们。
相邻帧应满足 `t_u24` 差 50 µs、`seq` 差 1。若某帧丢失，`seq` 差 2、`t_u24`
差 100 µs，按 `t_u24` 即可重建严格 20 kHz 网格。`benchlib.continuity()` 给出
`gaps`（不等 50 µs 的间隔数）、`max_gap_us`、`missing`（推算丢失帧数）与 `seq_ok`。

状态字：状态 `0..6` = IDLE/PRECHARGE/CALIBRATE/SAVE/RUN/FAULT/OFFSET；
故障 `0..12` = OK/SENSOR/ADC/TIMING/WINDOW/ALIGNMENT/FLASH/UART/BUS/ZERO/CURRENT/SPEED/POSITION；
功率模式 `0..2` = OFF/PRECHARGE/PWM。

### 四组通道（先发无法离线反算的量）

| 下标 | 组 0 raw | 组 1 current | 组 2 voltage | 组 3 control |
|---|---|---|---|---|
| 2 | `adc_raw_b` | `ib` A | `ud` V | `iq_ref` A |
| 3 | `adc_raw_c` | `ic` A | `uq` V | `iq_ref_cmd` A |
| 4 | `adc_raw_bus` | `elec_deg` ° | `ccr_a` | `pos_deg` ° |
| 5 | `v_b` V | `iq_ref` A | `ccr_b` | `pos_tgt` ° |
| 6 | `v_c` V | `integral_d` V | `ccr_c` | `rpm` |
| 7 | `v_bus` V | `integral_q` V | `duty_a` | `rpm_encoder` |
| 8 | `angle_raw_deg` ° | `ud` V | `duty_b` | `rpm_tgt` |
| 9 | `sample_us` | `uq` V | `duty_c` | `iq_outer` A |
| 10 | `bus_v_nominal` V | `b_offset` V | `edge_limit_v` V | `mode` |
| 11 | `b_offset` V | `c_offset` V | `vec_limit_v` V | `bus_v` V |

- **组 0**：ADC 原始码值与编码器**未修正**角度（二阶谐波补偿前的真值）。
  原始码值不是为了标定增益（软件无法自标定），而是把标定自由度留到将来：
  一旦有电流探头或已知负载，可以用同一批历史数据重算，不必重做实验。
  换算（`App/Hardware/bsp/bsp_adc.c`）：`v_b = adc_raw_b * 3.3/4095`，
  `v_bus = adc_raw_bus * (3.3/4095) * (41.2/2.2)`，
  `i = (v - offset) * 50`（标称，未标定）。
- **组 1**：PI 积分器与输出是电流环内部状态，主机无法从别处反算。
- **组 2**：`ccr_*` 是本周期实际生效的量化值（uint16 精确），电压余量对应
  `foc_modulate` 的采样窗口上限与 `Vbus/√3` 线性上限。
- **组 3**：`pos_deg`/`rpm` 是编码器 Ground Truth，进入无感 FOC 后仍然采集，
  用于与估计角度/速度对比。`mode`：0=Torque、1=Speed、2=Position。

同一工况在 4 个组上分别跑，`chain` 会按 `seq` 把四份数据对齐并导出宽表 CSV——
因为 12 float 的带宽装不下全部物理量，重复跑 + 离线拼接才是完整数据集。

## 命令

| 命令 | 含义 |
|---|---|
| `Iq <A>` | Torque 模式，目标 Iq，±5 A、最多两位小数 |
| `rpm <v>` | Speed 模式，目标转速，±9400 RPM |
| `pos <deg>` | Position 模式，**绝对**多圈机械角度，±1e6° |
| `zero` | 把当前位置定义为 0°（需停机且静止） |
| `stop` | 立即关断 |
| `clear` | 清已消失的故障，不自动启动 |
| `cal` | 停机重校准 |
| `send X` | 切换 20 kHz 日志组，X = 0..3（纯日志开关，与电机状态无关） |
| `hello` | **仅 UART** 回 `#FOC 1.1 <状态字>`；不发到 USB 二进制流，避免破坏分帧 |

三条模式命令**命令即切模式**，不需要额外的 mode 命令。速度环/位置环是 1 kHz
外环，最终输出 Iq 参考，底层仍是现有 20 kHz Id/Iq 电流环。

**看门狗**：Speed/Position 模式下若主机超过 200 ms 不再发目标，固件置
`FOC_UART` 停机——因为外环正握着计算出的参考值，PC 挂死必须能自停。
Torque 模式保持历史语义，没有该看门狗。所以主机必须周期性重发目标；
`benchlib.Scheduler` 默认 100 ms 重发一次，既保活又保证输入一致。

**参考斜坡**：Iq 参考按 1 A/s 从上一个值爬到新目标（20 kHz 定步长，实测
`iq - iq_prev == 5e-5 A`，即 1 A/s）。`stop` 清零并取消未完成的斜坡。
PC 只发一条 setpoint，斜坡由固件完成，因此同一工况的命令值在时间上可复现；
`meta.json.command_jitter_ms` 记录实际发送偏差供分析核对。

## 安全性

- 主机侧默认 `--iq-limit 0.4 A`、`--rpm-limit 1000`、`--pos-limit 1200°`，
  超限的工况在**发命令之前**就会被拒绝并打印 `SKIP`。
- 每个 (工况, 组) 采集结束后立刻检查归档：出现故障帧或 20 kHz 连续性丢失即
  终止整轮，不静默重试。
- 每次切换工况前执行固定前置序列（`stop` → 静置 → 位置模式 `zero`），
  保证同一工况跨组、跨重复的输入一致。
- 电机**无负载**固定台架；母线允许范围与电机额定范围不同，扩大幅值前先确认供电。

## 分析配方

- **电流/速度/位置闭环**：用组 3 的 `iq_ref_cmd`/`rpm_tgt`/`pos_tgt` 与
  `rpm`/`pos_deg` 算跟随误差、超调、静差、低速纹波。
- **Rs/Ld/Lq/磁链辨识**：组 0 的 `adc_raw_b/c/bus` + 组 1 的 `elec_deg`、
  `ud/uq`、组 2 的 `duty_*`；先在 `send 0` 下用静止注入或低速旋转取样。
- **机械参数（负载/惯量/摩擦）**：`send 0` 下施加已知 `Iq` 阶跃，用组 0 的
  未修正角度差分得快照速度，拟合加速段斜率。
- **无感 FOC 离线回放**：组 0（Vbus、原始电流码、真实角度）+ 组 2（实际施加的
  `duty`/`ccr`）足以在 PC 上重放电压与电流；组 3 提供目标轨迹与 Ground Truth 转速。
- **角度估计对比**：任何估计器都用组 0 的 `angle_raw_deg` 与组 3 的 `pos_deg`/
  `rpm` 作基准，注意前者是补偿前的真值。

未标定项：相电流绝对精度与增益、编码器内部测量延迟、`FOC_EDGE_LIMIT` 隐含的
模拟建立时间。不要把这些数字当成已标定事实。

## 自检

不接硬件也可以验证解析器：

```sh
python tools/bench/selftest.py
```

覆盖帧字节往返、拆分/粘连读取、错位重同步、丢帧检测、损坏拒绝、通道表、
归档往返与工况限值校验。
