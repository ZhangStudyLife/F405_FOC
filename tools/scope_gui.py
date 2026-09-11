#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scope_gui.py —— 编码器位置/速度实时波形上位机（不需要串口，走 ST-Link/SWD）。

    python tools/scope_gui.py

界面里能做什么
--------------
  * 实时滚动显示 6 路信号（默认：位置 / 速度 / 未滤波速度 / 原始计数 / CRC / 读耗时）
  * 窗口长度可调：样本越少刷新越快（刷新率受 SWD 带宽限制，见下）
  * 冻结：让目标板停下记录，保留**触发前**的波形（预触发捕获）
  * 自动缩放 / 通道开关
  * 存图（PNG）与导出（CSV）

刷新率说明
----------
实测 dump 24608 字节（6 通道 x 1024 样本）约 220 ms，即约 4.5 Hz。
这是 SWD 的物理带宽，不是脚本慢。把"窗口"调小可以直接提高刷新率：
256 样本约 55 ms（约 18 Hz），1024 样本约 220 ms（约 4.5 Hz）。
想同时要"看得快"和"存得多"，就平时用 256 窗口观察，需要细看时切到 1024。
"""

import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_lib import (ScopeReader, setup_cjk_font, parse_list,
                       DEFAULT_ELF, DEFAULT_NAMES, DEFAULT_DIVS)

WINDOW_CHOICES = ["128", "256", "512", "1024"]
DEFAULT_WINDOW = "256"


class ScopeApp:
    def __init__(self, root, elf=DEFAULT_ELF, symbol="g_scope",
                 names=None, divs=None, window=DEFAULT_WINDOW):
        self.root = root
        self.root.title("MT6835 位置/速度波形 —— ST-Link/SWD")
        self.root.geometry("1180x820")

        self.reader = None
        self.thread = None
        self.stop_flag = threading.Event()
        self.q = queue.Queue(maxsize=4)

        self.names = list(names) if names else list(DEFAULT_NAMES)
        self.divs = list(divs) if divs else list(DEFAULT_DIVS)
        self.window = int(window)
        self.ch_count = len(self.names)
        self.show = [True] * self.ch_count
        self.autoscale = tk.BooleanVar(value=True)
        self.frozen_now = False
        self.last_info = None
        self.last_samples = None
        self.frame_times = []

        cjk = setup_cjk_font()
        self.L_TIME = "时间 (ms)" if cjk else "time (ms)"
        self.L_HINT = "0 = 最新样本" if cjk else "0 = newest"

        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(30, self._poll)

    # ------------------------------------------------------------------
    # 界面
    # ------------------------------------------------------------------
    def _build_ui(self):
        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(side=tk.TOP, fill=tk.X)

        self.btn_run = ttk.Button(bar, text="▶ 开始", width=10,
                                  command=self._toggle_run)
        self.btn_run.pack(side=tk.LEFT)

        self.btn_freeze = ttk.Button(bar, text="❄ 冻结", width=10,
                                     command=self._toggle_freeze, state=tk.DISABLED)
        self.btn_freeze.pack(side=tk.LEFT, padx=(6, 0))

        ttk.Label(bar, text="  窗口(样本):").pack(side=tk.LEFT)
        self.cmb_window = ttk.Combobox(bar, values=WINDOW_CHOICES, width=6,
                                       state="readonly")
        self.cmb_window.set(str(self.window))
        self.cmb_window.pack(side=tk.LEFT)
        self.cmb_window.bind("<<ComboboxSelected>>", self._on_window_change)

        ttk.Checkbutton(bar, text="自动缩放", variable=self.autoscale
                        ).pack(side=tk.LEFT, padx=(12, 0))

        ttk.Button(bar, text="保存图片", command=self._save_png
                   ).pack(side=tk.RIGHT)
        ttk.Button(bar, text="导出 CSV", command=self._save_csv
                   ).pack(side=tk.RIGHT, padx=(0, 6))

        # 第二行：通道开关
        bar2 = ttk.Frame(self.root, padding=(8, 0, 8, 6))
        bar2.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(bar2, text="通道:").pack(side=tk.LEFT)
        self.ch_vars = []
        for i in range(self.ch_count):
            v = tk.BooleanVar(value=True)
            self.ch_vars.append(v)
            ttk.Checkbutton(bar2, text=self.names[i], variable=v,
                            command=self._on_channel_toggle
                            ).pack(side=tk.LEFT, padx=(4, 0))

        # 图形区
        self.fig = Figure(figsize=(11, 6.4), dpi=100)
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        self.axes = []
        self.lines = []
        for i in range(self.ch_count):
            ax = self.fig.add_subplot(self.ch_count, 1, i + 1)
            if i < self.ch_count - 1:
                ax.set_xticklabels([])
            ln, = ax.plot([], [], linewidth=0.9, color="C%d" % (i % 10))
            ax.set_ylabel(self.names[i])
            ax.grid(True, alpha=0.3)
            ax.margins(x=0)
            self.axes.append(ax)
            self.lines.append(ln)
        self.axes[-1].set_xlabel("%s      %s" % (self.L_TIME, self.L_HINT))
        self.fig.tight_layout()

        # 状态栏
        self.status = tk.StringVar(value="未开始。点「开始」连接 ST-Link。")
        ttk.Label(self.root, textvariable=self.status, relief=tk.SUNKEN,
                  anchor=tk.W, padding=(6, 3)).pack(side=tk.BOTTOM, fill=tk.X)

    # ------------------------------------------------------------------
    # 控制
    # ------------------------------------------------------------------
    def _toggle_run(self):
        if self.thread is None:
            self._start()
        else:
            self._stop()

    def _start(self):
        try:
            self.reader = ScopeReader(elf=DEFAULT_ELF)
            self.reader.start()
        except Exception as e:
            self.reader = None
            messagebox.showerror("连接失败", str(e))
            self.status.set("连接失败: %s" % e)
            return

        self.stop_flag.clear()
        self.thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.thread.start()
        self.btn_run.config(text="■ 停止")
        self.btn_freeze.config(state=tk.NORMAL)
        self.status.set("已连接 g_scope @ 0x%08X，采集中…" % self.reader.addr)

    def _stop(self):
        self.stop_flag.set()
        if self.thread is not None:
            self.thread.join(timeout=2.0)
            self.thread = None
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        self.btn_run.config(text="▶ 开始")
        self.btn_freeze.config(state=tk.DISABLED, text="❄ 冻结")
        self.frozen_now = False
        self.status.set("已停止。")

    def _toggle_freeze(self):
        if self.reader is None:
            return
        self.frozen_now = not self.frozen_now
        try:
            self.reader.set_frozen(self.frozen_now)
        except Exception as e:
            messagebox.showerror("冻结失败", str(e))
            self.frozen_now = False
            return
        self.btn_freeze.config(text="▶ 解冻" if self.frozen_now else "❄ 冻结")

    def _on_window_change(self, _evt=None):
        self.window = int(self.cmb_window.get())

    def _on_channel_toggle(self):
        self.show = [v.get() for v in self.ch_vars]
        for i, ln in enumerate(self.lines):
            ln.set_visible(self.show[i] and i < self.ch_count)
        self.canvas.draw_idle()

    # ------------------------------------------------------------------
    # 后台取数
    # ------------------------------------------------------------------
    def _reader_loop(self):
        while not self.stop_flag.is_set():
            t0 = time.time()
            try:
                info, samples = self.reader.read(window=self.window)
            except Exception as e:
                self.q.put(("err", str(e)))
                return
            dt = time.time() - t0
            try:
                self.q.put(("data", info, samples, dt), timeout=1.0)
            except queue.Full:
                pass   # 主线程没跟上就丢帧，不阻塞取数

    # ------------------------------------------------------------------
    # 主线程刷新
    # ------------------------------------------------------------------
    def _poll(self):
        try:
            while True:
                item = self.q.get_nowait()
                if item[0] == "err":
                    self.status.set("取数出错: %s" % item[1])
                    self._stop()
                    break
                _, info, samples, dt = item
                self._update(info, samples, dt)
        except queue.Empty:
            pass
        self.root.after(30, self._poll)

    def _update(self, info, samples, dt):
        self.last_info = info
        self.last_samples = samples

        n = len(samples)
        if n == 0:
            self.status.set("缓冲里还没有样本（目标板还没开始 scope_push？）")
            return

        fs = max(info["fs"], 1)
        t = [(i + 1 - n) * 1000.0 / fs for i in range(n)]
        ch = info["channels"]

        for c in range(min(ch, self.ch_count)):
            y = [row[c] / self.divs[c] for row in samples]
            self.lines[c].set_data(t, y)
            ax = self.axes[c]
            ax.set_xlim(t[0], 0)
            if self.autoscale.get():
                lo, hi = min(y), max(y)
                if hi - lo < 1e-9:
                    hi, lo = lo + 1.0, lo - 1.0     # 平线时给个可视范围
                pad = (hi - lo) * 0.1
                ax.set_ylim(lo - pad, hi + pad)

        self.canvas.draw_idle()

        # 刷新率统计
        self.frame_times.append(dt)
        if len(self.frame_times) > 20:
            self.frame_times.pop(0)
        avg = sum(self.frame_times) / len(self.frame_times)
        self.status.set(
            "刷新 %.1f Hz (单帧 %.0f ms) | %d 样本 @ %u Hz = %.0f ms 窗口 | "
            "环形已绕 %u 圈%s"
            % (1.0 / avg if avg > 0 else 0.0, avg * 1000, n, fs,
               n * 1000.0 / fs, info["total"] // max(info["depth"], 1),
               "  ❄已冻结" if info["frozen"] else ""))

    # ------------------------------------------------------------------
    # 导出
    # ------------------------------------------------------------------
    def _save_png(self):
        if self.last_samples is None:
            messagebox.showinfo("没有数据", "先点「开始」采一帧。")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".png", filetypes=[("PNG 图片", "*.png")],
            initialfile="scope.png")
        if not path:
            return
        self.fig.savefig(path, dpi=120)
        self.status.set("已保存图片: %s" % path)

    def _save_csv(self):
        if self.last_samples is None:
            messagebox.showinfo("没有数据", "先点「开始」采一帧。")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".csv", filetypes=[("CSV", "*.csv")],
            initialfile="scope.csv")
        if not path:
            return
        info, samples = self.last_info, self.last_samples
        fs = max(info["fs"], 1)
        n = len(samples)
        with open(path, "w", encoding="utf-8") as f:
            f.write("t_ms," + ",".join(self.names[:info["channels"]]) + "\n")
            for i, row in enumerate(samples):
                t = (i + 1 - n) * 1000.0 / fs
                vals = [row[c] / self.divs[c] for c in range(len(row))]
                f.write("%.4f," % t + ",".join("%.6g" % v for v in vals) + "\n")
        self.status.set("已导出 CSV: %s" % path)

    def _on_close(self):
        self.stop_flag.set()
        if self.thread is not None:
            self.thread.join(timeout=2.0)
        if self.reader is not None:
            self.reader.stop()
        self.root.destroy()


def main():
    import argparse
    ap = argparse.ArgumentParser(description="MT6835 位置/速度实时波形上位机")
    ap.add_argument("--names", default=None, help="通道名，逗号分隔")
    ap.add_argument("--div", default=None, help="每通道除数，逗号分隔")
    ap.add_argument("--window", default=DEFAULT_WINDOW, choices=WINDOW_CHOICES,
                    help="显示窗口长度（样本数），越小刷新越快")
    args = ap.parse_args()

    names = parse_list(args.names, len(DEFAULT_NAMES), DEFAULT_NAMES, cast=str)
    divs = parse_list(args.div, len(DEFAULT_DIVS), DEFAULT_DIVS, cast=float)
    divs = [(d if d else 1.0) for d in divs]

    root = tk.Tk()
    ScopeApp(root, names=names, divs=divs, window=args.window)
    root.mainloop()


if __name__ == "__main__":
    main()
