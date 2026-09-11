# tools —— 主机侧工具

| 文件 | 用途 |
|---|---|
| **`scope_gui.py`** | **波形上位机（带界面，实时刷新）** ← 平时用这个 |
| `run_scope_gui.bat` | 双击启动上面那个界面 |
| `scope_capture.py` | 命令行抓一次并存图/CSV，适合脚本化 |
| `scope_lib.py` | 两者共用的取数底层（OpenOCD telnet + 解析） |

---

## scope_gui.py —— 波形上位机

**双击 `run_scope_gui.bat`**，或命令行：

```
python tools\scope_gui.py
```

打开后点「开始」，即可看到实时刷新的波形。**不需要串口、不需要 SWO，只用 SWD。**

### 界面能做什么

- 实时显示**位置与速度**（默认只开这两路，其余通道可勾选）
- **Y 轴固定**：默认位置 `0~360` 度、速度 `-200~200` rpm，界面上可直接改
- **窗口长度可调**：样本越少刷新越快（见下表）
- **冻结（空格键）**：让目标板停止记录，保留**触发前**的波形 —— 抓瞬态用这个
- 存图片（PNG）、导出 CSV

### Y 轴为什么默认固定

自动缩放会让纵轴范围每帧重算：平线时范围塌缩成一条缝、有毛刺时又猛跳，
看上去像波形自己在上下弹，根本没法观察趋势。所以默认**关闭自动缩放**，
改用固定范围。

| 通道 | 默认范围 | 依据 |
|---|---|---|
| 位置 | `0 ~ 360` 度 | 编码器输出本来就是 `[0,360)`，范围天然固定 |
| 速度 | `-200 ~ 200` rpm | 手转电机一般在 ±150 rpm 以内，取 ±200 留余量 |

跑 FOC 高速时要自己往上调 —— 在界面「Y 轴范围」那一行改，按回车生效。
命令行也能指定：

```
python tools\scope_gui.py --ypos 0,360 --yspd -3000,3000
```

### 手转电机时"存下来是平的"？

这是早期版本的坑，已经修掉：

- 默认窗口原来只有 256 样本 @1 kHz = **0.256 秒**，手转一圈根本装不下
  → 现在默认 **1024 样本（1.024 秒）**
- 原来不冻结就导出，等你停下转动再点导出，缓冲早滚过去了
  → 现在**点导出/存图会自动先冻结**，保证存到的就是你眼前这一帧
- 另外**空格键**随时冻结 —— 一边转一边敲，抓瞬态最方便

### 刷新率（实测，SWD 带宽决定）

| 窗口 | 单帧耗时 | 刷新率 |
|---|---|---|
| 1024 样本 | 237 ms | 4.2 Hz |
| 256 样本 | 59 ms | **16.9 Hz** |
| 128 样本 | 33 ms | **30.4 Hz** |

这是 SWD 的物理带宽，不是脚本慢。**平时用 256 观察（约 17 Hz 足够看趋势），
需要细看某段时切到 1024。**

### 默认通道布局（与 `main.c` 对应）

| 通道 | 含义 | 除数 |
|---|---|---|
| ch0 | 位置，毫度 `[0, 360000)` | 1000 → 度 |
| ch1 | 速度，rpm×100（已低通） | 100 → rpm |
| ch2 | 速度，rpm×100（未滤波） | 100 → rpm |
| ch3 | 原始 21 bit 计数值 | 1 |
| ch4 | 芯片给的 CRC | 1 |
| ch5 | 同步阻塞读耗时 ns | 1 |

转动电机轴时应该看到：**ch0 是平滑锯齿**（每圈从 0 爬到 360 再落回 0）；
**ch1 是平滑曲线**，数值与实际转速相符；**ch2 有明显毛刺** —— 那是 21 位角度
量化带来的噪声，正好用它判断低通该设多重。

改了 `main.c` 的通道分配后，用 `--names` / `--div` 覆盖：

```
python tools\scope_gui.py --names ia,ib,ic,angle,id,iq --div 1,1,1,1000,100,100
```

---

## scope_capture.py —— 命令行抓取

抓一次、存图、退出。适合写进脚本或自动化。

### 原理

目标板的 `App/Protocols/scope` 模块在控制中断里以全速（例如 20 kHz）把信号
写进 RAM 环形缓冲；本工具用 OpenOCD 的 `dump_image` 把整块内存搬回来，
按固定布局解析后绘图。

为什么不"实时读变量"：SWD 单次读变量要几毫秒，顶多采到几百 Hz，而 FOC 是
20 kHz —— 那样看到的只是混叠后的假波形。反过来让目标板自己采、主机事后整块搬，
时间分辨率才是真的。

### 用法

```bash
python tools/scope_capture.py                       # 抓一次并存图
python tools/scope_capture.py --freeze              # 先冻结再抓（保留触发前历史）
python tools/scope_capture.py --save fig.png        # 指定图片路径
python tools/scope_capture.py --csv cap.csv         # 顺便存 CSV
python tools/scope_capture.py --names ia,ib,ic      # 自定义通道名
python tools/scope_capture.py --div 1,1,1           # 自定义除数
python tools/scope_capture.py --repeat 5 --interval 2
python tools/scope_capture.py --window 256          # 只取最近 256 样本（更快）
```

| 参数 | 说明 |
|---|---|
| `--elf` | ELF 路径，默认 `build/Release/405_FOC.elf` |
| `--symbol` | scope 实例符号名，默认 `g_scope` |
| `--names` | 通道名，逗号分隔。默认按 `main.c` 的 bring-up 布局 |
| `--div` | 每通道除数，把放大成整数的通道除回真实单位 |
| `--csv` | 导出 CSV（`--repeat` 时自动加序号后缀） |
| `--save` | 图片输出路径，默认 `scope.png` |
| `--show` | 弹交互窗口（命令行下用；平时建议直接用 GUI） |
| `--window` | 只取最近 N 个样本 |
| `--freeze` / `--resume` | 冻结 / 解冻目标板缓冲 |
| `--repeat` / `--interval` | 重复抓取次数与间隔 |

### 前置条件

1. 固件里定义了全局 `scope_t g_scope;` 并调过 `scope_init()`
2. 已编译 Release：`foc.sh build Release`（脚本要读 ELF 的符号表拿地址）
3. ST-Link 已连接

### 两个已踩过的坑

- **`dump binary memory` 的路径不能加引号**，也不能用相对路径。GDB 的 Windows
  版会把引号当成文件名的一部分，报 `No such file or directory`。脚本里统一用
  绝对路径 + 正斜杠 + 不加引号，并显式挑了不含空格的目录。
- **OpenOCD 的 telnet 端口 4444 在本机被 Windows 保留端口段占用**
  （见 `netsh int ipv4 show excludedportrange protocol=tcp`，4382~4481）。
  脚本启动 OpenOCD 时带了 `-c "telnet_port disabled"` 绕开。
