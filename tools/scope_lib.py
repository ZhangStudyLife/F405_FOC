#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scope_lib.py —— RAM 示波器的取数底层，供 CLI（scope_capture.py）和
GUI（scope_gui.py）共用。

取数走 OpenOCD 的 telnet 接口而不是每次起一个 GDB：
  - GDB 路线：每次抓取都要 fork 一个 gdb 进程，约 1 秒开销
  - telnet 路线：OpenOCD 常驻，一条 dump_image 命令读完 24 KB 约 220 ms

实测（24608 字节 / 6 通道 x 1024 样本）：约 220 ms，即约 4.5 Hz 刷新。
这是 SWD 的带宽上限，不是脚本的锅。
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

# --------------------------------------------------------------------------
# 工具链路径（与 .claude/skills/foc-build-flash 里固化的那套一致）
# --------------------------------------------------------------------------
OCD = r"D:\DevEnv\openocd-v0.12.0-i686-w64-mingw32\bin\openocd.exe"
OCD_SCRIPTS = r"D:\DevEnv\openocd-v0.12.0-i686-w64-mingw32\share\openocd\scripts"
GDB = r"D:\DevEnv\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe"
NM = r"D:\DevEnv\GNU-tools-for-STM32\bin\arm-none-eabi-nm.exe"

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(PROJECT_ROOT, "build", "Release", "405_FOC.elf")

# 示波器实例是 App/Protocols/scope/scope_encoder.c 里的文件作用域静态变量，
# 属于那个模块的实现细节 —— 所以是 s_ 前缀而不是 g_。
# 它是静态符号，nm 一样能列出来（小写 b/B）。
DEFAULT_SYMBOL = "s_scope"

SCOPE_MAGIC = 0x53434F50          # "SCOP"
HEADER_WORDS = 8
HEADER_SIZE = HEADER_WORDS * 4
VERSION = 1

# 本机 Windows 保留端口段包含 4382~4481，OpenOCD 默认的 telnet 4444 正好落在
# 里面会导致启动即失败，所以换一个空闲端口。
DEFAULT_TELNET_PORT = 5555


def exe(path):
    """Windows 上补 .exe 后缀（os.path.isfile 不会自动补）。"""
    if os.path.isfile(path):
        return path
    if os.path.isfile(path + ".exe"):
        return path + ".exe"
    return path


def find_symbol(elf, name):
    """从 ELF 符号表取变量地址。"""
    nm = exe(NM)
    if not os.path.isfile(nm):
        raise RuntimeError("找不到 arm-none-eabi-nm: " + nm)
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] == name:
            return int(p[0], 16)
    return None


# --------------------------------------------------------------------------
# CRC / 解码辅助（主机侧用不到，但 GUI 想显示带宽时可以留）
# --------------------------------------------------------------------------

def linearize(flat, ch, dp, write_index, count):
    """环形缓冲 -> 时间递增的样本列表。"""
    count = min(count, dp, write_index)
    start = (write_index & (dp - 1)) if write_index >= dp else 0
    out = []
    for i in range(count):
        s = ((start + i) & (dp - 1)) * ch
        out.append(flat[s:s + ch])
    return out


# --------------------------------------------------------------------------
# OpenOCD telnet 会话
# --------------------------------------------------------------------------
class OcdTelnet:
    """一条常驻的 OpenOCD telnet 连接。

    比"每次起一个 GDB"快一个数量级 —— GDB 进程启动本身就要约 1 秒。
    """

    def __init__(self, port=DEFAULT_TELNET_PORT):
        self.port = port
        self.sock = None

    def connect(self, timeout=10.0):
        self.sock = socket.create_connection(("127.0.0.1", self.port),
                                             timeout=timeout)
        self.sock.settimeout(timeout)
        # OpenOCD 会先吐一段欢迎信息，以 "> " 结尾；读到它就算连上了。
        # 不要用固定 sleep —— 之前那版等了整整一个 timeout，白白多花 10 秒。
        self._read_until_prompt(timeout=timeout)

    def _read_until_prompt(self, timeout=20.0):
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                d = self.sock.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            if buf.rstrip().endswith(b">"):
                break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=20.0):
        if self.sock is None:
            raise RuntimeError("telnet 未连接")
        self.sock.sendall((line + "\n").encode())
        return self._read_until_prompt(timeout)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


# --------------------------------------------------------------------------
# 读取器
# --------------------------------------------------------------------------
class ScopeReader:
    """管好 OpenOCD 进程 + telnet 连接 + 临时文件的生命周期。"""

    def __init__(self, elf=DEFAULT_ELF, symbol=DEFAULT_SYMBOL,
                 port=DEFAULT_TELNET_PORT):
        self.elf = elf
        self.symbol = symbol
        self.port = port
        self.addr = None
        self.proc = None
        self.tel = None
        self._dump_path = os.path.join(tempfile.gettempdir(), "dsh_scope.bin")
        self._lock = threading.Lock()

    # ---- 生命周期 ----
    def start(self):
        if not os.path.isfile(self.elf):
            raise RuntimeError("找不到 ELF: %s\n先编译: foc.sh build Release"
                               % self.elf)
        self.addr = find_symbol(self.elf, self.symbol)
        if self.addr is None:
            raise RuntimeError(
                "ELF 里没有符号 %s。确认固件里定义了 scope_t %s; 且已重新编译。"
                % (self.symbol, self.symbol))

        ocd = exe(OCD)
        if not os.path.isfile(ocd):
            raise RuntimeError("找不到 openocd: " + ocd)
        self.proc = subprocess.Popen(
            [ocd,
             "-f", os.path.join(OCD_SCRIPTS, "interface", "stlink.cfg"),
             "-f", os.path.join(OCD_SCRIPTS, "target", "stm32f4x.cfg"),
             "-c", "telnet_port %d" % self.port,
             # GUI 只用 telnet；把 gdb 端口关掉，免得和别的调试会话抢
             "-c", "gdb_port disabled"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # 等 telnet 端口就绪（轮询连接，不固定 sleep）
        deadline = time.time() + 15.0
        last = None
        while time.time() < deadline:
            try:
                self.tel = OcdTelnet(self.port)
                self.tel.connect(timeout=5.0)
                return
            except OSError as e:
                last = e
                self.tel = None
                time.sleep(0.3)
        self.stop()
        raise RuntimeError("OpenOCD telnet 连不上（检查 ST-Link）: %s" % last)

    def stop(self):
        if self.tel is not None:
            self.tel.close()
            self.tel = None
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None

    # ---- 冻结 / 解冻（预触发捕获）----
    def set_frozen(self, frozen):
        self.tel.cmd("mww 0x%08X %d" % (self.addr + 28, 1 if frozen else 0))

    # ---- 读一次 ----
    def read(self, window=None):
        """抓一帧。

        window: 只取最近多少个样本。None = 整个环形缓冲。
                实时界面用小的 window 能显著提高刷新率 —— dump 的字节数
                直接正比于样本数，1024 样本要 220 ms，256 样本只要约 55 ms。
        返回 (info, samples)；samples 是按时间递增排列的通道列表。
        """
        with self._lock:
            # 1) 头部：8 个字，一条 mdw 就够，几乎不花时间
            out = self.tel.cmd("mdw 0x%08X %d" % (self.addr, HEADER_WORDS))
            words = []
            for line in out.splitlines():
                if ":" in line:
                    try:
                        words += [int(x, 16) for x in line.split(":", 1)[1].split()]
                    except ValueError:
                        pass
            if len(words) < HEADER_WORDS:
                raise RuntimeError("读不到 scope 头部，OpenOCD 返回:\n" + out)

            magic, version, ch, dp, fs, wi, total, frozen = words[:8]

            if magic != SCOPE_MAGIC:
                raise RuntimeError(
                    "magic=0x%08X 不是 0x%08X，符号可能不是 scope 实例"
                    % (magic, SCOPE_MAGIC))
            if ch == 0 or dp == 0 or ch > 64 or dp > (1 << 20):
                raise RuntimeError("头部字段不合理: channels=%d depth=%d" % (ch, dp))

            # 2) 决定读多少样本
            avail = min(wi, dp)
            count = avail if window is None else min(window, avail)
            if count == 0:
                return self._info(magic, version, ch, dp, fs, wi, total,
                                  frozen, 0), []

            # 3) 算物理地址范围。环形会回绕，最多拆成两段来读。
            start_logical = wi - count
            start_phys = start_logical & (dp - 1)
            head = min(count, dp - start_phys)      # 第一段长度（不跨环尾）
            tail = count - head                     # 第二段从环首开始

            flat = []
            if head > 0:
                flat += self._dump_words(start_phys * ch, head * ch)
            if tail > 0:
                flat += self._dump_words(0, tail * ch)

            samples = [flat[i * ch:(i + 1) * ch] for i in range(count)]
            return self._info(magic, version, ch, dp, fs, wi, total,
                              frozen, count), samples

    @staticmethod
    def _info(magic, version, ch, dp, fs, wi, total, frozen, count):
        return {
            "magic": magic, "version": version, "channels": ch, "depth": dp,
            "fs": fs, "write_index": wi, "total": total,
            "frozen": bool(frozen), "count": count,
        }

    def _dump_words(self, word_offset, word_count):
        """从实例数据区第 word_offset 个 int32 起读 word_count 个。"""
        base = self.addr + HEADER_SIZE + word_offset * 4
        if os.path.exists(self._dump_path):
            os.unlink(self._dump_path)

        # dump_image 的路径用花括号包住（OpenOCD 的 TCL 语法），
        # 这样即使路径带空格也没问题。注意别用引号 —— 那是 GDB 的坑。
        self.tel.cmd("dump_image {%s} 0x%08X %d"
                     % (self._dump_path, base, word_count * 4), timeout=30.0)

        if not os.path.exists(self._dump_path):
            raise RuntimeError("dump_image 没有产生文件（地址越界？）")
        with open(self._dump_path, "rb") as f:
            raw = f.read()
        os.unlink(self._dump_path)

        need = word_count * 4
        if len(raw) < need:
            raise RuntimeError("只读到 %d 字节，需要 %d" % (len(raw), need))
        return list(struct.unpack_from("<%di" % word_count, raw, 0))


# --------------------------------------------------------------------------
# matplotlib 中文字体
# --------------------------------------------------------------------------
def setup_cjk_font():
    """让 matplotlib 能画中文。找不到中文字体返回 False。"""
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


# 与 main.c 的 bring-up 通道分配对应
DEFAULT_NAMES = ["pos_deg", "speed_rpm", "speed_raw_rpm",
                 "raw", "crc", "blocking_ns"]
DEFAULT_DIVS = [1000.0, 100.0, 100.0, 1.0, 1.0, 1.0]


def parse_list(s, count, default, cast=float):
    """把 "a,b,c" 解析成长度 count 的列表，不足的用 default 补。"""
    if s:
        vals = [cast(x.strip()) for x in s.split(",") if x.strip() != ""]
    else:
        vals = list(default)
    while len(vals) < count:
        vals.append(default[len(vals)] if len(vals) < len(default)
                    else (1.0 if cast is float else "ch%d" % len(vals)))
    return vals[:count]
