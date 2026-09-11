# tools —— 主机侧工具

## scope_capture.py

通过 ST-Link / SWD 抓取目标板 RAM 里的"示波器"缓冲并绘图。
**不需要串口，不需要 SWO，只用 SWD。**

### 原理

目标板的 `App/Protocols/scope` 模块在控制中断里以全速（例如 20 kHz）把信号
写进 RAM 环形缓冲；本脚本用 GDB 的 `dump binary memory` 一次性把整块内存搬回来，
按固定布局解析后绘图。

为什么不"实时读变量"：SWD 单次读变量要几毫秒，顶多采到几百 Hz，而 FOC 是
20 kHz —— 那样看到的只是混叠后的假波形。反过来让目标板自己采、主机事后整块搬，
时间分辨率才是真的。

### 用法

```bash
python tools/scope_capture.py                       # 抓一次并弹图
python tools/scope_capture.py --freeze              # 先冻结再抓（保留触发前历史）
python tools/scope_capture.py --save fig.png        # 存图不弹窗
python tools/scope_capture.py --csv cap.csv         # 顺便存 CSV
python tools/scope_capture.py --names ia,ib,ic,angle,id,iq
python tools/scope_capture.py --repeat 5 --interval 2
```

| 参数 | 说明 |
|---|---|
| `--elf` | ELF 路径，默认 `build/Release/405_FOC.elf` |
| `--symbol` | scope 实例符号名，默认 `g_scope` |
| `--names` | 通道名，逗号分隔；不给就是 `ch0..chN` |
| `--csv` | 导出 CSV（`--repeat` 时自动加序号后缀） |
| `--save` | 保存图片（给了就不弹窗） |
| `--freeze` | 抓之前把 `frozen` 置 1，保留触发前最近 N 个样本 |
| `--resume` | 抓完解冻，恢复记录 |
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
