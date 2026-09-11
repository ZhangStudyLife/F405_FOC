#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scope_capture.py —— 通过 ST-Link / SWD 抓取目标板 RAM 里的"示波器"缓冲并绘图。

不需要串口，不需要 SWO，只用 SWD。

原理
----
目标板上的 App/Protocols/scope 模块在控制中断里以全速（例如 20 kHz）把信号
写进 RAM 环形缓冲；本脚本用 GDB 的 dump binary memory 一次性把整块内存搬回来，
按固定布局解析后绘图。

为什么不"实时读变量"：SWD 单次读变量要几毫秒，顶多采到几百 Hz，
而 FOC 是 20 kHz —— 那样看到的是混叠后的假波形。反过来让目标板自己采、
主机事后整块搬，时间分辨率才是真的。

用法
----
    python tools/scope_capture.py                     # 抓一次并弹图
    python tools/scope_capture.py --csv cap.csv       # 顺便存 CSV
    python tools/scope_capture.py --save fig.png      # 存图不弹窗
    python tools/scope_capture.py --freeze            # 先冻结再抓（保留触发前历史）
    python tools/scope_capture.py --repeat 5 --interval 2
    python tools/scope_capture.py --names ia,ib,ic,angle,id,iq
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

# Windows 控制台默认可能是 GBK，中文提示会变成乱码
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# --------------------------------------------------------------------------
# 工具链路径（与 .claude/skills/foc-build-flash 里固化的那套保持一致）
# --------------------------------------------------------------------------
OCD = r"D:\DevEnv\openocd-v0.12.0-i686-w64-mingw32\bin\openocd.exe"
OCD_SCRIPTS = r"D:\DevEnv\openocd-v0.12.0-i686-w64-mingw32\share\openocd\scripts"
GDB = r"D:\DevEnv\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe"
NM = r"D:\DevEnv\GNU-tools-for-STM32\bin\arm-none-eabi-nm.exe"


def exe(path):
    """Windows 上补 .exe 后缀（os.path.isfile 不会自动补）。"""
    if os.path.isfile(path):
        return path
    if os.path.isfile(path + ".exe"):
        return path + ".exe"
    return path

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(PROJECT_ROOT, "build", "Release", "405_FOC.elf")
SYMBOL = "g_scope"

SCOPE_MAGIC = 0x53434F50          # "SCOP"
HEADER_WORDS = 8                  # 8 个 uint32 = 32 字节
HEADER_SIZE = HEADER_WORDS * 4
VERSION = 1


def die(msg):
    print("错误: " + msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# 符号查找
# --------------------------------------------------------------------------
def find_symbol(elf, name):
    """从 ELF 符号表里取变量地址。没找到返回 None。"""
    nm = exe(NM)
    if not os.path.isfile(nm):
        die("找不到 arm-none-eabi-nm: " + nm)
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


# --------------------------------------------------------------------------
# OpenOCD
# --------------------------------------------------------------------------
class OpenOCD:
    def __init__(self, elf):
        self.proc = None
        self.elf = elf

    def start(self):
        ocd = exe(OCD)
        if not os.path.isfile(ocd):
            die("找不到 openocd: " + ocd)
        cmd = [
            ocd,
            "-f", os.path.join(OCD_SCRIPTS, "interface", "stlink.cfg"),
            "-f", os.path.join(OCD_SCRIPTS, "target", "stm32f4x.cfg"),
            # 本机 Windows 保留端口段包含 4382~4481，OpenOCD 默认的 telnet 4444
            # 正好落在里面，会直接启动失败。本脚本只用 GDB 端口，把它关掉。
            "-c", "telnet_port disabled",
        ]
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        time.sleep(2.5)
        if self.proc.poll() is not None:
            die("OpenOCD 启动失败（检查 ST-Link 连接）")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# --------------------------------------------------------------------------
# 抓取
# --------------------------------------------------------------------------
def gdb_run(script_lines, timeout=90):
    """跑一段 GDB 批处理脚本，返回输出。"""
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False,
                                     encoding="utf-8") as f:
        f.write("\n".join(script_lines) + "\n")
        path = f.name
    try:
        r = subprocess.run([exe(GDB), "-q", "-batch", "-x", path],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout + r.stderr
    finally:
        os.unlink(path)


def pick_dump_path():
    """挑一个能用的临时文件路径。

    有两个坑必须绕开：
      1. GDB(Windows 版) 的 `dump binary memory` **不接受带引号的路径** ——
         加了引号会把引号当成文件名的一部分，报 "No such file or directory"。
      2. 相对路径也不行：GDB 的工作目录不是 Python 的当前目录。

    所以必须用绝对路径、正斜杠、不加引号。代价是路径里不能有空格
    （用户名带空格时会踩到），因此这里显式挑一个没有空格的目录。
    """
    candidates = [
        os.path.join(tempfile.gettempdir(), "dsh_scope.bin"),
        os.path.join(PROJECT_ROOT, "..", "tmp", "scope_dump.bin"),
        os.path.join(PROJECT_ROOT, "scope_dump.bin"),
    ]
    for c in candidates:
        c = os.path.abspath(c)
        if " " in c:
            continue
        try:
            os.makedirs(os.path.dirname(c), exist_ok=True)
        except OSError:
            continue
        return c
    die("找不到不含空格的临时目录来放 dump 文件")


def capture(elf, addr, freeze=False, resume=False, quiet=False):
    """把 scope 结构体整块读回来，返回 (header_dict, samples)。"""
    dump_path = pick_dump_path()
    if os.path.exists(dump_path):
        os.unlink(dump_path)

    lines = [
        "set pagination off",
        "set confirm off",
        "set auto-load off",
        "set complaints 0",
        # file 必须在 target 之前，否则 GDB 会在无符号表时报错并丢掉后续命令
        "file " + elf.replace("\\", "/"),
        "target extended-remote localhost:3333",
    ]
    if freeze:
        lines.append("set *(unsigned int*)0x%X = 1" % (addr + 28))
    if resume:
        lines.append("set *(unsigned int*)0x%X = 0" % (addr + 28))

    # 先读头部的 channels/depth，再据此决定搬多少字节 —— 这样主机脚本
    # 完全不需要知道目标板配了几通道、多深。
    lines += [
        "set $ch = *(unsigned int*)(0x%X + 8)" % addr,
        "set $dp = *(unsigned int*)(0x%X + 12)" % addr,
        "set $mg = *(unsigned int*)(0x%X + 0)" % addr,
        'printf "SCOPEHDR %u %u %u\\n", $mg, $ch, $dp',
        # 路径不加引号、用正斜杠 —— 见 pick_dump_path() 的说明
        "dump binary memory %s 0x%X (0x%X + %d + $ch * $dp * 4)"
        % (dump_path.replace("\\", "/"), addr, addr, HEADER_SIZE),
        "detach",
        "monitor resume",   # 不 resume 的话芯片会一直停着
        "quit",
    ]

    out = gdb_run(lines)

    m = re.search(r"SCOPEHDR\s+(\d+)\s+(\d+)\s+(\d+)", out)
    if not m:
        die("读不到 scope 头部，GDB 输出:\n" + out.strip()[-800:])

    magic, ch, dp = int(m.group(1)), int(m.group(2)), int(m.group(3))

    if magic != SCOPE_MAGIC:
        die("magic = 0x%08X，不是 0x%08X。"
            "多半是 g_scope 没定义、没初始化，或者抓错了符号。" % (magic, SCOPE_MAGIC))
    if ch == 0 or dp == 0 or ch > 64 or dp > (1 << 20):
        die("头部字段不合理: channels=%d depth=%d" % (ch, dp))

    if not os.path.exists(dump_path) or os.path.getsize(dump_path) == 0:
        die("GDB 没能写出 dump 文件 %s\nGDB 输出:\n%s"
            % (dump_path, out.strip()[-800:]))

    with open(dump_path, "rb") as f:
        raw = f.read()
    os.unlink(dump_path)

    need = HEADER_SIZE + ch * dp * 4
    if len(raw) < need:
        die("只读到 %d 字节，需要 %d 字节（内存越界？）" % (len(raw), need))

    hdr = struct.unpack_from("<8I", raw, 0)
    version = hdr[1]
    if version != VERSION:
        print("警告: 目标版本 %d，本脚本期望 %d，字段含义可能已变"
              % (version, VERSION), file=sys.stderr)

    flat = struct.unpack_from("<%di" % (ch * dp), raw, HEADER_SIZE)

    write_index = hdr[5]
    total = hdr[6]

    # 环形缓冲整理成时间递增序列：最老的样本在前
    count = min(write_index, dp)
    start = (write_index & (dp - 1)) if write_index >= dp else 0
    samples = []
    for i in range(count):
        s = ((start + i) & (dp - 1)) * ch
        samples.append(flat[s:s + ch])

    info = {
        "channels": ch,
        "depth": dp,
        "fs": hdr[4],
        "write_index": write_index,
        "total": total,
        "frozen": hdr[7],
        "count": count,
        "version": version,
    }
    if not quiet:
        print("抓到 %d 个样本 x %d 通道，采样率 %u Hz"
              % (count, ch, hdr[4]))
        if total > dp:
            print("  (环形已绕 %u 圈，缓冲里是最近 %.1f ms)"
                  % (total // dp, count * 1000.0 / max(hdr[4], 1)))
        if hdr[7]:
            print("  (已冻结 —— 下面波形的时间零点就是触发点)")
    return info, samples


# --------------------------------------------------------------------------
# 输出
# --------------------------------------------------------------------------
def save_csv(path, info, samples, names):
    fs = max(info["fs"], 1)
    n = len(samples)
    with open(path, "w", encoding="utf-8") as f:
        f.write("t_ms," + ",".join(names) + "\n")
        for i, row in enumerate(samples):
            t = (i + 1 - n) * 1000.0 / fs      # 最新样本在 t=0，便于对齐触发点
            f.write("%.4f," % t + ",".join(str(v) for v in row) + "\n")
    print("已写出 CSV: " + path)


def setup_cjk_font():
    """让 matplotlib 能画中文。找不到中文字体就返回 False，标签退回英文。"""
    try:
        import matplotlib
        from matplotlib import font_manager
    except ImportError:
        return False
    for name in ["Microsoft YaHei", "SimHei", "Noto Sans CJK SC",
                 "Source Han Sans SC", "PingFang SC"]:
        try:
            font_manager.findfont(name, fallback_to_default=False)
        except Exception:
            continue
        matplotlib.rcParams["font.sans-serif"] = [name, "DejaVu Sans"]
        matplotlib.rcParams["axes.unicode_minus"] = False
        return True
    return False


def plot(info, samples, names, save=None, show=False):
    """绘图。

    默认**只存图不弹窗** —— 这是刻意的：matplotlib 的交互后端在没有显示环境
    （脚本、CI、SSH、tkinter 未安装）时，plt.show() 会永久阻塞，
    而且不报错、不超时，只表现为"卡住"。要弹窗必须显式 --show。
    """
    try:
        import matplotlib
    except ImportError:
        print("没装 matplotlib，只能输出 CSV。", file=sys.stderr)
        return

    if show:
        # 后端必须在 import pyplot 之前选好，之后再 use() 不生效
        interactive = False
        for backend in ("TkAgg", "QtAgg", "Qt5Agg", "GTK3Agg", "WXAgg"):
            try:
                matplotlib.use(backend, force=True)
                import matplotlib.pyplot as plt          # noqa: F401
                interactive = True
                break
            except Exception:
                continue
        if not interactive:
            print("没有可用的交互后端（多半是没装 tkinter），改为存图。",
                  file=sys.stderr)
            matplotlib.use("Agg")
    else:
        interactive = False
        matplotlib.use("Agg")

    import matplotlib.pyplot as plt

    cjk = setup_cjk_font()
    L_TIME = "时间 (ms)" if cjk else "time (ms)"
    L_HINT = "0 = 最新样本 / 触发点" if cjk else "0 = newest sample / trigger"
    L_FROZEN = "  [已冻结]" if cjk else "  [frozen]"

    fs = max(info["fs"], 1)
    n = len(samples)
    ch = info["channels"]

    # 时间轴：最新样本落在 t = 0（冻结时就是触发点），往前是历史
    t = [(i + 1 - n) * 1000.0 / fs for i in range(n)]

    fig, axes = plt.subplots(ch, 1, sharex=True, figsize=(12, 1.6 * ch + 1))
    if ch == 1:
        axes = [axes]

    for c in range(ch):
        y = [row[c] for row in samples]
        ax = axes[c]
        ax.plot(t, y, linewidth=0.9, color="C%d" % (c % 10))
        ax.set_ylabel(names[c])
        ax.grid(True, alpha=0.3)
        ax.margins(x=0)

    axes[-1].set_xlabel("%s    %s" % (L_TIME, L_HINT))
    title = "%d ch @ %u Hz" % (ch, info["fs"])
    if info["frozen"]:
        title += L_FROZEN
    axes[0].set_title(title)
    fig.tight_layout()

    if interactive:
        plt.show()
    else:
        out = save if save else "scope.png"
        fig.savefig(out, dpi=120)
        print("波形已保存: " + os.path.abspath(out))


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="通过 SWD 抓取目标板 RAM 示波器缓冲并绘图")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="ELF 路径")
    ap.add_argument("--symbol", default=SYMBOL, help="scope 实例的符号名")
    ap.add_argument("--names", default=None,
                    help="通道名，逗号分隔，例如 ia,ib,ic,angle,id,iq")
    ap.add_argument("--csv", default=None, help="保存 CSV")
    ap.add_argument("--save", default=None,
                    help="图片输出路径，默认 scope.png")
    ap.add_argument("--show", action="store_true",
                    help="弹出交互窗口（默认只存图；无显示环境会阻塞，故为可选项）")
    ap.add_argument("--freeze", action="store_true", help="抓之前先冻结")
    ap.add_argument("--resume", action="store_true", help="抓完解冻（恢复记录）")
    ap.add_argument("--repeat", type=int, default=1, help="重复抓取次数")
    ap.add_argument("--interval", type=float, default=2.0, help="重复间隔（秒）")
    args = ap.parse_args()

    if not os.path.isfile(args.elf):
        die("找不到 ELF: " + args.elf + "\n先编译: foc.sh build Release")

    addr = find_symbol(args.elf, args.symbol)
    if addr is None:
        die("ELF 里没有符号 %s。确认固件里定义了 scope_t %s; 并且已重新编译。"
            % (args.symbol, args.symbol))
    print("符号 %s @ 0x%08X" % (args.symbol, addr))

    ocd = OpenOCD(args.elf)
    ocd.start()
    try:
        last = None
        for k in range(max(args.repeat, 1)):
            if args.repeat > 1:
                print("\n--- 第 %d/%d 次 ---" % (k + 1, args.repeat))
            info, samples = capture(args.elf, addr,
                                    freeze=args.freeze and k == 0,
                                    resume=args.resume,
                                    quiet=(args.repeat > 1 and k > 0))
            last = (info, samples)

            names = (args.names.split(",") if args.names
                     else ["ch%d" % i for i in range(info["channels"])])
            if len(names) < info["channels"]:
                names += ["ch%d" % i
                          for i in range(len(names), info["channels"])]

            if args.csv:
                if args.repeat > 1:
                    root, ext = os.path.splitext(args.csv)
                    path = "%s_%d%s" % (root, k, ext)
                else:
                    path = args.csv
                save_csv(path, info, samples, names)

            if args.interval and k < args.repeat - 1:
                time.sleep(args.interval)

        if last:
            names = (args.names.split(",") if args.names
                     else ["ch%d" % i for i in range(last[0]["channels"])])
            if len(names) < last[0]["channels"]:
                names += ["ch%d" % i
                          for i in range(len(names), last[0]["channels"])]
            plot(last[0], last[1], names,
                 save=args.save, show=args.show)
    finally:
        ocd.stop()


if __name__ == "__main__":
    main()
