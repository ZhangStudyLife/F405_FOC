#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scope_capture.py —— 通过 ST-Link/SWD 抓一帧 RAM 示波器数据并存图/CSV。

不需要串口，不需要 SWO，只用 SWD。

日常看波形请用带界面的 tools/scope_gui.py；这个脚本适合抓一次、导出、
或者写进自动化流程。

    python tools/scope_capture.py                    # 抓一帧存 scope.png
    python tools/scope_capture.py --csv cap.csv      # 顺便导出 CSV
    python tools/scope_capture.py --freeze           # 先冻结再抓（保留触发前历史）
    python tools/scope_capture.py --window 256       # 只取最近 256 样本（更快）
    python tools/scope_capture.py --names ia,ib,ic --div 1,1,1

取数底层见 scope_lib.py。
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_lib import (ScopeReader, setup_cjk_font, parse_list,
                       DEFAULT_ELF, DEFAULT_NAMES, DEFAULT_DIVS)


def die(msg):
    print("错误: " + msg, file=sys.stderr)
    sys.exit(1)


def save_csv(path, info, samples, names, divs):
    fs = max(info["fs"], 1)
    n = len(samples)
    with open(path, "w", encoding="utf-8") as f:
        f.write("t_ms," + ",".join(names) + "\n")
        for i, row in enumerate(samples):
            t = (i + 1 - n) * 1000.0 / fs      # 最新样本在 t=0，便于对齐触发点
            vals = [row[c] / divs[c] for c in range(len(row))]
            f.write("%.4f," % t + ",".join("%.6g" % v for v in vals) + "\n")
    print("已写出 CSV: " + path)


def plot(info, samples, names, divs, save=None, show=False):
    """绘图。

    默认**只存图不弹窗**：matplotlib 的交互后端在没有显示环境
    （脚本、CI、SSH、tkinter 未安装）时 plt.show() 会永久阻塞，
    而且不报错、不超时，只表现为"卡住"。要弹窗必须显式 --show。
    """
    try:
        import matplotlib
    except ImportError:
        print("没装 matplotlib，只输出 CSV。", file=sys.stderr)
        return

    interactive = False
    if show:
        # 后端必须在 import pyplot 之前选好，之后再 use() 不生效
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
    if not interactive:
        matplotlib.use("Agg")

    import matplotlib.pyplot as plt

    cjk = setup_cjk_font()
    L_TIME = "时间 (ms)" if cjk else "time (ms)"
    L_HINT = "0 = 最新样本 / 触发点" if cjk else "0 = newest / trigger"
    L_FROZEN = "  [已冻结]" if cjk else "  [frozen]"

    fs = max(info["fs"], 1)
    n = len(samples)
    ch = info["channels"]
    t = [(i + 1 - n) * 1000.0 / fs for i in range(n)]

    fig, axes = plt.subplots(ch, 1, sharex=True, figsize=(12, 1.6 * ch + 1))
    if ch == 1:
        axes = [axes]

    for c in range(ch):
        y = [row[c] / divs[c] for row in samples]
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


def main():
    ap = argparse.ArgumentParser(
        description="通过 SWD 抓取目标板 RAM 示波器缓冲（日常看波形建议用 scope_gui.py）")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="ELF 路径")
    ap.add_argument("--symbol", default="g_scope", help="scope 实例的符号名")
    ap.add_argument("--names", default=None,
                    help="通道名，逗号分隔。默认: " + ",".join(DEFAULT_NAMES))
    ap.add_argument("--div", default=None,
                    help="每通道除数。默认: "
                         + ",".join("%g" % d for d in DEFAULT_DIVS))
    ap.add_argument("--window", type=int, default=None,
                    help="只取最近 N 个样本（越小越快；默认取整个环形缓冲）")
    ap.add_argument("--csv", default=None, help="保存 CSV")
    ap.add_argument("--save", default=None, help="图片输出路径，默认 scope.png")
    ap.add_argument("--show", action="store_true", help="弹出交互窗口")
    ap.add_argument("--freeze", action="store_true", help="抓之前先冻结目标板")
    ap.add_argument("--resume", action="store_true", help="抓完解冻")
    ap.add_argument("--repeat", type=int, default=1, help="重复抓取次数")
    ap.add_argument("--interval", type=float, default=2.0, help="重复间隔（秒）")
    args = ap.parse_args()

    reader = ScopeReader(elf=args.elf, symbol=args.symbol)
    try:
        reader.start()
    except Exception as e:
        die(str(e))
    print("符号 %s @ 0x%08X" % (args.symbol, reader.addr))

    last = None
    names = list(DEFAULT_NAMES)
    divs = list(DEFAULT_DIVS)
    try:
        for k in range(max(args.repeat, 1)):
            if args.repeat > 1:
                print("\n--- 第 %d/%d 次 ---" % (k + 1, args.repeat))
            if args.freeze:
                reader.set_frozen(True)

            try:
                info, samples = reader.read(window=args.window)
            except Exception as e:
                die(str(e))

            if args.resume:
                reader.set_frozen(False)

            last = (info, samples)
            ch = info["channels"]
            names = parse_list(args.names, ch, DEFAULT_NAMES, cast=str)
            divs = parse_list(args.div, ch, DEFAULT_DIVS, cast=float)
            divs = [(d if d else 1.0) for d in divs]

            print("抓到 %d 个样本 x %d 通道，采样率 %u Hz"
                  % (len(samples), ch, info["fs"]))
            if info["total"] > info["depth"]:
                print("  (环形已绕 %u 圈，缓冲里是最近 %.1f ms)"
                      % (info["total"] // info["depth"],
                         len(samples) * 1000.0 / max(info["fs"], 1)))
            if info["frozen"]:
                print("  (已冻结 —— 波形时间零点就是触发点)")

            if args.csv:
                if args.repeat > 1:
                    root, ext = os.path.splitext(args.csv)
                    save_csv("%s_%d%s" % (root, k, ext),
                             info, samples, names, divs)
                else:
                    save_csv(args.csv, info, samples, names, divs)

            if args.interval and k < args.repeat - 1:
                import time
                time.sleep(args.interval)

        if last:
            plot(last[0], last[1], names, divs,
                 save=args.save, show=args.show)
    finally:
        reader.stop()


if __name__ == "__main__":
    main()
