# 台架停机转速冻结修复报告

日期：2026-09-26

## 根因

`stop` 将 FOC 状态切换为 `IDLE`，随后 `foc_outer_step()` 不再调用 `control_step()`。`control_speed_rpm()` 因此停留在停机瞬间的旧值；本次日志中的 `244.45 rpm` 就是冻结值，实际 UART 反馈已接近零。bench 的 `wait_idle()` 等待 USB 转速低于阈值，所以每次恢复都会重复失败。

## 修复

- `foc_outer_step()` 在所有状态调用测量更新。
- `control_step()` 在 `IDLE`/非 `RUN` 状态只更新速度和位置，不计算速度/位置控制输出。
- 保持 `stop` 的关 PWM、清零 Iq、清积分行为不变。
- 增加主机回归：运行后 stop，模拟转子滑行，验证 USB 速度回到零且位置继续更新。

## 验证

- 主机测试 `test_app_usb`：通过，新增 idle coast 回归通过。
- `test_control_pid`、`test_foc_recalibration`：通过。
- Debug、Release、`FOC_CAPTURE=ON` 构建：通过。
- bench selftest、recovery test、`git diff --check`：通过。
- Release 已重新通过 ST-Link 烧录和校验。
- `speed_cross_zero` 24 V 连续两次：通过，0 丢帧、0 故障帧。
- 全量 39 工况，24/18/12 V：全部执行完成，耗时 577.92 s，通电 577.09 s；1,048,597 帧，0 丢帧，0 故障帧。

结果分类：18 项“通过”，9 项“已采集”，12 项“跟踪失败”。跟踪失败集中在 5/25 rpm、方波和位置轨迹，属于控制跟踪指标未达标，不再导致流程中止。

## 收尾

- USB：COM17，`state=IDLE`、`fault=0`、`power=0`、`rpm≈0`。
- CH340：COM14，UART 状态 `IDLE`、无故障、`rpm≈0`。
- 学生电源：COM16，DPS-150/V1.0，输出关闭，保护状态 0。
- Release BIN SHA-256：`25354d0485ad6bda4b4989d09348a42969395529b63d6e6ec30bf7ddc5329d3c`。
- 原始归档和统计：`build/bench_debug/20260926_idle_fix/20260926_141131/`、`summary.json`、`final.json`。

未覆盖：本轮未执行板上 FOC_CAPTURE 采满、停机和导出，仅完成 Capture 构建验证。
