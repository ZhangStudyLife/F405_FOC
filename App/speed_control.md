# 速度与位置控制

当前实现位于 `Control/control.c`，参数位于 `Config/motor_config.h` 和 `Config/motor_params.h`。

## 控制路径

- 编码器 PLL 每 50 µs 更新，仅使用通过保护检查的角度增量；带宽参数为 2000 rad/s。
- 速度 PI 和位置 P 每 1 ms 更新。位置误差乘 Kp 后按 `position_speed` 限幅，作为速度目标。
- 速度环采用增量二自由度 PI，参考权重 β=0.8：
  `Δu = Kp·Δ(β·目标速度−反馈速度) + Ki·dt·(目标速度−反馈速度)`。
- 加入角度前馈后，完整 Iq 目标先按 ±20 A 限幅，再按 120 A/s 限变率；
  积分状态反算跟踪最终目标。`current_ramp` 仅用于转矩命令。
- `foc.rpm` 单独用于电角速度预测和超速保护；遥测速度为外环 PLL 的 `control.speed`。

源码默认速度 Kp=0.005、Ki=0.01，位置 Kp=4，位置速度上限=100 rpm。
启动时优先加载有效 Flash 参数；修改源码默认值不会覆盖已保存的参数。
运行参数的单位、范围及 `set`/`save` 行为见 [README](README.md)。

## 角度前馈

`Control/cogging_table.inc` 是 512 点只读电流表，按未经谐波补偿的绝对机械角度线性插值。
在 |速度|≤50 rpm 时全量加入，在 50～200 rpm 间线性退出，≥200 rpm 时为零。
`zero` 不改变其角度索引。

现有表来自历史 `baseline_low.csv`：双向各四圈，前三圈用于拟合和重复性筛选，
第四圈独立验证。离线拟合结果不能证明闭环运动性能，也不能证明所有周期项均为真实齿槽转矩。
50～200 rpm 的退出区间仍为实验参数。

## 主机工具

在固件工程目录执行，Python 需安装 pyserial、NumPy 和 SciPy：

```powershell
python tools/speed_control.py cog
python tools/speed_control.py fit build/bench_debug/<目录>/cog.csv
python tools/speed_control.py accept
python tools/speed_control.py report build/bench_debug/<目录>/accept.csv
```

`cog`/`accept` 会实际驱动电机，使用匹配串口身份和现有 20 通道解析器，
输出 CSV、串口原始字节与停机记录到 `build/bench_debug/<时间戳>/`。
它们不自动校准或清错；当前板卡需已校准且无故障。
重新标定须使用未开启该前馈的基线固件；`fit` 会覆盖编译表，需重新构建。
`report` 的 Iq 波动统计不作滤波或扣除前馈；原始角度导数仅作相同方法的对照。
主机接收时间不是精确 MCU 采样时间。

当前源码尚未完成低速、高速及完整启停验收。历史测试出现过编码器欠压告警，
诊断与保护行为见 [MT6835](../docs/mt6835.md)；旧版本统计不代表当前版本性能。
