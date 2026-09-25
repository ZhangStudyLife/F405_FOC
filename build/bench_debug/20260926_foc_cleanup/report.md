# FOC 清理与台架验证报告

日期：2026-09-26（Asia/Shanghai）

## 修改

- 新增 `App/Config/motor_config.h`，集中 5010 电机与 MT6835 安装参数。
- FOC 和速度/位置环读取静态参数；删除无调用的积分器读取接口和位置保持接口。
- 删除已确认不再构建或测试依赖的 `tests/legacy` 三个旧电压模式文件。
- 修正主机测试文档，使新增配置目录可被独立 GCC 命令找到。
- 删除 FOC 中遗留未使用变量。

## 构建与测试

- Debug 构建：通过，无编译警告。
- Release 构建：通过，无编译警告。
- `FOC_CAPTURE=ON` 独立构建：通过，RAM 106088 B，CCMRAM 64 KB，FLASH 62144 B。
- 六项主机 C 测试：全部通过。
- `tools/bench/selftest.py`：通过。
- `tools/bench/test_recovery.py`：4 项通过。
- `git diff --check`：通过。

## 烧录与设备收尾

- ST-Link：`8600A1002031363534313541`，设备 ID `0x413`，1 MB，Release 烧录和校验成功并复位。
- MCU USB：`COM17`，序列号 `3862385E3335`。
- 复位后连续遥测：`state=IDLE`、`fault=0`、`Iq_ref=0`、`rpm=0`。
- 学生电源：`COM16`，`DPS-150`，序列号 `V1.0`，输出关闭，保护状态 0，输出电流 0 A。
- Release BIN SHA-256：`711f3a99fff9a701d61769c01201bab02d7d1393dfae72311182e782d3516cbd`。

## 未覆盖项

- 本轮未执行带电电机低速、高速、正反转、速度/位置工况；因此不能宣称这些台架验收项通过。
- 本轮未执行板上 FOC_CAPTURE 采满、停机和 UART 导出；仅完成 `FOC_CAPTURE=ON` 编译验证。
- 烧录前旧固件曾出现固定 `rpm=7000` 的陈旧遥测值，虽 `IDLE/无故障/Iq_ref=0` 且电源已关闭，仍按保守原则未据此宣称静止；Release 复位后该值恢复为 0。

数据与产物：`build/Release/405_FOC.bin`、`build/FOC_CAPTURE/405_FOC.bin`，本报告目录。
