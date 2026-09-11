#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scope_gui.py —— 编码器位置/速度实时波形上位机（不需要串口，走 ST-Link/SWD）。

    python tools/scope_gui.py          （或双击 run_scope_gui.bat）

界面里能做什么
--------------
  * 实时显示位置与速度（默认只开这两路，其余通道可勾选）
  * **Y 轴固定**：默认位置 0~360 度、速度 -200~200 rpm，可在界面上改
  * 窗口长度可调：样本越少刷新越快
  * **冻结（空格键）**：让目标板停下记录，保留触发前的波形
  * 存图（PNG）/ 导出（CSV）

关于 Y 轴为什么要固定
--------------------
自动缩放会让纵轴范围每帧重算 —— 平线时范围塌缩成一条缝、有毛刺时又猛跳，
看上去像波形在上下弹，根本没法观察趋势。所以默认**关闭自动缩放**并使用固定范围。

关于手转电机时"存下来是平的"
------------------------------
原来的默认窗口只有 256 样本 @1 kHz = 0.256 秒，手转一圈装不下；
而且不冻结就导出，等你停下转动再点导出，缓冲早滚过去了。
现在：
  * 默认窗口改成 1024（1.024 秒，够装下手转的一圈）
  * 点导出/存图时**自动先冻结**，保证存到的就是你眼前这一帧
  * 空格键随时冻结 —— 一边转一边按，抓最方便
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

WINDOW_CHOICES = ["256", "512", "1024"]
DEFAULT_WINDOW = "1024"

# 默认只显示这两路：位置、速度
DEFAULT_VISIBLE = [0, 1]

# 固定 Y 轴默认范围（按物理量纲定，不是从数据里猜的）：
#   位置：编码器输出就是 [0,360) 度，范围天然固定
#   速度：手转电机一般在 ±150 rpm 以内，取 ±200 留余量；
#         跑 FOC 高速时需要自己往上调，界面上可以改
DEFAULT_YRANGE = {
    0: (0.0, 360.0),      # 位置 (度)
    1: (-200.0, 200.0),   # 速度 (rpm)
}


class ScopeApp:
    def __init__(self, root, elf=DEFAULT_ELF, symbol="g_scope",
                 names=None, divs=None, window=DEFAULT_WINDOW,
                 visible=None, yrange=None):
        self.root = root
        self.root.title("MT6835 位置/速度波形 —— ST-Link/SWD")
        self.root.geometry("1180x760")

        self.reader = None
        self.thread = None
        self.stop_flag = threading.Event()
        self.q = queue.Queue(maxsize=4)

        self.names = list(names) if names else list(DEFAULT_NAMES)
        self.divs = list(divs) if divs else list(DEFAULT_DIVS)
        self.window = int(window)
        self.ch_count = len(self.names)
        self.show = [False] * self.ch_count
        for i in (visible if visible is not None else DEFAULT_VISIBLE):
            if 0 <= i < self.ch_count:
                self.show[i] = True
        self.yrange = dict(yrange if yrange is not None else DEFAULT_YRANGE)
        self.autoscale = tk.BooleanVar(value=False)   # 默认固定 Y 轴
        self.frozen_now = False
        self.last_info = None
        self.last_samples = None
        self.frame_times = []

        self.axes = [None] * self.ch_count
        self.lines = [None] * self.ch_count
        self.yentries = {}

        cjk = setup_cjk_font()
        self.L_TIME = "时间 (ms)" if cjk else "time (ms)"
        self.L_HINT = "0 = 最新样本" if cjk else "0 = newest"

        self._build_ui()
        self._rebuild_axes()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(30, self._poll)

    # ------------------------------------------------------------------
    # 界面
    # ------------------------------------------------------------------
    def _build_ui(self):
        # 第一行：运行控制
        bar = ttk.Frame(self.root, padding=(8, 6, 8, 0))
        bar.pack(side=tk.TOP, fill=tk.X)

        self.btn_run = ttk.Button(bar, text="▶ 开始", width=10, takefocus=0,
                                  command=self._toggle_run)
        self.btn_run.pack(side=tk.LEFT)

        self.btn_freeze = ttk.Button(bar, text="❄ 冻结 (空格)", width=14,
                                     takefocus=0, command=self._toggle_freeze,
                                     state=tk.DISABLED)
        self.btn_freeze.pack(side=tk.LEFT, padx=(6, 0))

        ttk.Label(bar, text="  窗口(样本):").pack(side=tk.LEFT)
        self.cmb_window = ttk.Combobox(bar, values=WINDOW_CHOICES, width=6,
                                       state="readonly")
        self.cmb_window.set(str(self.window))
        self.cmb_window.pack(side=tk.LEFT)
        self.cmb_window.bind("<<ComboboxSelected>>", self._on_window_change)

        ttk.Checkbutton(bar, text="自动缩放 Y", variable=self.autoscale,
                        command=self._redraw).pack(side=tk.LEFT, padx=(12, 0))

        ttk.Button(bar, text="导出 CSV", takefocus=0, command=self._save_csv
                   ).pack(side=tk.RIGHT)
        ttk.Button(bar, text="保存图片", takefocus=0, command=self._save_png
                   ).pack(side=tk.RIGHT, padx=(0, 6))

        # 第二行：通道开关
        bar2 = ttk.Frame(self.root, padding=(8, 4, 8, 0))
        bar2.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(bar2, text="显示通道:").pack(side=tk.LEFT)
        self.ch_vars = []
        for i in range(self.ch_count):
            v = tk.BooleanVar(value=self.show[i])
            self.ch_vars.append(v)
            ttk.Checkbutton(bar2, text=self.names[i], variable=v,
                            command=self._on_channel_toggle
                            ).pack(side=tk.LEFT, padx=(6, 0))

        # 第三行：Y 轴固定范围
        bar3 = ttk.Frame(self.root, padding=(8, 4, 8, 6))
        bar3.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(bar3, text="Y 轴范围:").pack(side=tk.LEFT)
        for idx in sorted(self.yrange.keys()):
            if idx >= self.ch_count:
                continue
            ttk.Label(bar3, text="  %s" % self.names[idx]).pack(side=tk.LEFT)
            lo = tk.StringVar(value="%g" % self.yrange[idx][0])
            hi = tk.StringVar(value="%g" % self.yrange[idx][1])
            e_lo = ttk.Entry(bar3, textvariable=lo, width=8)
            e_hi = ttk.Entry(bar3, textvariable=hi, width=8)
            e_lo.pack(side=tk.LEFT, padx=(2, 0))
            ttk.Label(bar3, text="~").pack(side=tk.LEFT)
            e_hi.pack(side=tk.LEFT)
            for e in (e_lo, e_hi):
                e.bind("<Return>", self._apply_yrange)
                e.bind("<FocusOut>", self._apply_yrange)
            self.yentries[idx] = (lo, hi)
        ttk.Label(bar3, text="   （取消勾选「自动缩放 Y」后生效；改完按回车）"
                  ).pack(side=tk.LEFT)

        # 图形区
        self.fig = Figure(figsize=(11, 5.6), dpi=100)
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        # 状态栏
        self.status = tk.StringVar(value="未开始。点「开始」连接 ST-Link。")
        ttk.Label(self.root, textvariable=self.status, relief=tk.SUNKEN,
                  anchor=tk.W, padding=(6, 3)).pack(side=tk.BOTTOM, fill=tk.X)

        # 空格键 = 冻结/解冻。一边用手转电机一边敲空格最顺手。
        # 按钮设了 takefocus=0，否则点了按钮之后空格会去触发那个按钮。
        self.root.bind("<space>", self._on_space)

    def _on_space(self, _evt):
        if self.reader is not None:
            self._toggle_freeze()
        return "break"

    # ------------------------------------------------------------------
    # 坐标轴（随通道开关动态重建）
    # ------------------------------------------------------------------
    def _rebuild_axes(self):
        self.fig.clear()
        self.axes = [None] * self.ch_count
        self.lines = [None] * self.ch_count

        visible = [i for i in range(self.ch_count) if self.show[i]]
        if not visible:
            self.canvas.draw_idle()
            return

        n = len(visible)
        for k, i in enumerate(visible):
            ax = self.fig.add_subplot(n, 1, k + 1)
            if k < n - 1:
                ax.set_xticklabels([])
            ln, = ax.plot([], [], linewidth=0.9, color="C%d" % (i % 10))
            ax.set_ylabel(self.names[i])
            ax.grid(True, alpha=0.3)
            ax.margins(x=0)
            rng = self.yrange.get(i)
            if rng:
                ax.set_ylim(rng)
            self.axes[i] = ax
            self.lines[i] = ln

        self.axes[visible[-1]].set_xlabel("%s      %s" % (self.L_TIME, self.L_HINT))
        self.fig.tight_layout()
        self.canvas.draw_idle()

    def _apply_yrange(self, _evt=None):
        for idx, (v_lo, v_hi) in self.yentries.items():
            try:
                lo = float(v_lo.get())
                hi = float(v_hi.get())
            except ValueError:
                continue          # 输入非法就忽略，保留上一次的范围
            if hi <= lo:
                hi = lo + 1.0
            self.yrange[idx] = (lo, hi)
        self._redraw()

    def _redraw(self):
        for i in range(self.ch_count):
            ax = self.axes[i]
            if ax is None:
                continue
            rng = self.yrange.get(i)
            if self.autoscale.get():
                ax.relim()
                ax.autoscale_view(scalex=False, scaley=True)
            elif rng:
                ax.set_ylim(rng)
        self.canvas.draw_idle()

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
        self.status.set("已连接 g_scope @ 0x%08X，采集中…   （空格键 = 冻结）"
                        % self.reader.addr)

    def _stop(self):
        self.stop_flag.set()
        if self.thread is not None:
            self.thread.join(timeout=2.0)
            self.thread = None
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        self.btn_run.config(text="▶ 开始")
        self.btn_freeze.config(state=tk.DISABLED, text="❄ 冻结 (空格)")
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
        self.btn_freeze.config(text="▶ 解冻 (空格)" if self.frozen_now
                               else "❄ 冻结 (空格)")
        if self.frozen_now:
            self.status.set("❄ 已冻结 —— 缓冲停在触发瞬间，现在导出/存图都是这一帧。"
                            "（空格解冻）")

    def _on_window_change(self, _evt=None):
        self.window = int(self.cmb_window.get())

    def _on_channel_toggle(self):
        self.show = [v.get() for v in self.ch_vars]
        self._rebuild_axes()

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
                pass   # 主线程没跟上就丢帧，绝不阻塞取数

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
            ax = self.axes[c]
            if ax is None:               # 该通道被隐藏
                continue
            y = [row[c] / self.divs[c] for row in samples]
            self.lines[c].set_data(t, y)
            ax.set_xlim(t[0], 0)
            if self.autoscale.get():
                lo, hi = min(y), max(y)
                if hi - lo < 1e-9:
                    hi, lo = lo + 1.0, lo - 1.0     # 平线时给个可视范围
                pad = (hi - lo) * 0.1
                ax.set_ylim(lo - pad, hi + pad)
            else:
                rng = self.yrange.get(c)
                if rng:
                    ax.set_ylim(rng)

        self.canvas.draw_idle()

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
    # 导出：先冻结，保证存到的就是眼前这一帧
    # ------------------------------------------------------------------
    def _ensure_frozen(self):
        if self.reader is None or self.last_samples is None:
            return False
        if not self.frozen_now:
            try:
                self.reader.set_frozen(True)
                self.frozen_now = True
                self.btn_freeze.config(text="▶ 解冻 (空格)")
                # 冻结后再取一帧，确保屏幕上的数据和要保存的一致
                self.last_info, self.last_samples = self.reader.read(window=self.window)
                self._update(self.last_info, self.last_samples, 0.001)
            except Exception as e:
                messagebox.showerror("冻结失败", str(e))
                return False
            self.status.set("导出前已自动冻结，保证存到的就是这一帧。（空格解冻）")
        return True

    def _save_png(self):
        if not self._ensure_frozen():
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
        if not self._ensure_frozen():
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
        self.status.set("已导出 CSV: %s  （按空格解冻继续采集）" % path)

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
    ap.add_argument("--ypos", default=None,
                    help="位置 Y 轴范围 lo,hi（默认 0,360）")
    ap.add_argument("--yspd", default=None,
                    help="速度 Y 轴范围 lo,hi（默认 -200,200）")
    args = ap.parse_args()

    names = parse_list(args.names, len(DEFAULT_NAMES), DEFAULT_NAMES, cast=str)
    divs = parse_list(args.div, len(DEFAULT_DIVS), DEFAULT_DIVS, cast=float)
    divs = [(d if d else 1.0) for d in divs]

    yrange = dict(DEFAULT_YRANGE)
    for key, val, idx in (("ypos", args.ypos, 0), ("yspd", args.yspd, 1)):
        if val:
            try:
                lo, hi = [float(x) for x in val.split(",")]
                yrange[idx] = (lo, hi)
            except ValueError:
                print("警告: --%s 格式应为 lo,hi，已忽略" % key, file=sys.stderr)

    root = tk.Tk()
    ScopeApp(root, names=names, divs=divs, window=args.window, yrange=yrange)
    root.mainloop()


if __name__ == "__main__":
    main()
