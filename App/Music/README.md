# 电机歌曲：birthdaySong 与 V1 数据协议

`Music birthdaySong` 播放用户提供的完整八音盒生日歌录音，共 **36.864 秒**。
数据由 MP3 实际解码生成，保留整段时间轴、伴奏、叠音、强弱和衰减尾音。
转换仅混合单声道、抗混叠重采样、去直流、整体归一化，并在首尾各加 20 ms 渐变；不改变速度和音高。
原文件及 SHA256 写在 [birthdaySong.inc](birthdaySong.inc) 开头，便于核对来源。

录音在 20 kHz 单声道解码后的频谱分析中，约 99.864% 能量在 2 kHz 以下。
因此选择 **10 kHz / 12 位有符号 PCM**，以较小量化误差存完整波形；5 kHz 以上信息和立体声空间感不保留。
板端四点插值到既有 20 kHz 电流环，不进行 MP3 解码，不按音符重建歌曲。
电流波形保留音源细节；电机最终声音仍取决于电流跟随与机械频响，本版尚未实机试听。

## 串口命令协议

USART2：1000000 baud、8N1，无流控；ASCII 文本，CR 或 LF 结束，CRLF 只执行一次。

| 命令 | 行为 |
|---|---|
| `Music birthdaySong` | D 轴播放一次生日歌，从头开始；再次发送重新播放 |
| `music birthdaySong` | 同上；歌曲名区分大小写 |
| `Music stop` / `music stop` | 停止音乐，清除音乐 Id/Iq，保留运动目标与功率状态 |
| `music play` | 保留原来的 C4～C5 八音阶，4.8 秒 |
| `music play q` | 保留 2 A 峰值 Q 轴音阶对照，仅接受零转矩且转速低于 5 rpm |
| `stop` | 关闭功率，清除音乐和运动目标 |

播放要求零偏、电角度校准有效，状态为 IDLE 或 RUN。IDLE 时进入原有预充电并以 Iq=0 启动；
RUN 时保留原速度、位置或转矩目标。故障、校准、预充电或零偏采集中拒绝播放。
歌曲结束只将音乐目标归零，FOC 继续 RUN；需要关闭功率时发送 `stop`。
故障、停止、重新初始化和开始校准会终止播放。

板子继续只输出原有 JustFloat。通道 16 为命令计数，17 为结果：1 成功、2 未知歌曲或语法错误、3 状态不允许。
未知歌曲不会打断正在播放的音乐。`music.h` 的 `MUSIC_ENABLE=0` 排除音乐数据及实现，并拒绝播放。

## Flash 数据协议 V1

每首歌曲独立存为一个 `.inc`，包含只读字节数组和 `music_song_t` 描述：

```c
static const uint8_t birthdaySong_data[] = { /* 打包 PCM 字节 */ };
static const music_song_t birthdaySong = {
    "birthdaySong", birthdaySong_data, 368640u
};
```

| 字段 / 约定 | 含义 |
|---|---|
| `name` | 串口中的歌曲名，必须是 C 标识符 |
| `data` | 单声道，固定 10000 Hz，12 位二进制补码，范围 −2048～2047 |
| `samples` | 有效 10 kHz 样本数，不含填充；时长为 samples / 10000 秒 |
| 两样本 / 三字节 | sample 0 的低 8 位；sample 0 高 4 位与 sample 1 低 4 位；sample 1 高 8 位 |
| 边界填充 | 前置两个零样本，尾部至少四个零样本；总样本数补齐偶数 |
| 幅值 | 解码整数 / 2048；转换工具将实际插值峰值归一化到约 2000 / 2048 |

打包两个 12 位补码整数 `a`、`b`（先取 `& 0xfff`）：

```text
byte[0] = a & 0xff
byte[1] = (a >> 8) | ((b & 0x0f) << 4)
byte[2] = b >> 4
```

数组编译到 `.rodata`，直接从 Flash 读取，不占整首歌曲大小的 RAM，运行中不擦写 Flash。
歌曲数据占 **552969 字节**。程序区容量仍为 768 KiB；0x080C0000～0x080FFFFF 的参数与校准区保留。
一秒歌曲约占 15000 字节；添加歌曲时全部数据须同时放进程序区，超出容量会链接失败。

## 转换和添加后续歌曲

电脑需要 FFmpeg、Python 和 NumPy。在工程根目录运行：

```powershell
python App/Music/convert_song.py "D:\Downloads\歌曲.mp3" anotherSong
```

工具将完整音源输出为 `App/Music/anotherSong.inc`。随后在 `music.c`：

```c
#include "birthdaySong.inc"
#include "anotherSong.inc"
static const music_song_t *const songs[] = {&birthdaySong, &anotherSong};
```

重新构建、烧录后发送 `Music anotherSong`。串口只选取编入固件的歌曲，不传输音频文件。

```powershell
cmake --preset Release
cmake --build --preset Release
python ../download/flash.py flash Release --erase=program
```

烧录只擦程序区以保留校准。烧录和实际电机试听尚未在本次执行。

## 播放与验证

每个 50 µs 周期顺序读取波形，D 轴幅值上限为 `sqrt(max(0, 20² − Iq_motion²))` A。
运动 Iq 不因音乐而降低；到 ±20 A 时音乐目标为零，时间轴仍继续。
R/L 前馈使用下一 PWM 中心的波形值及中心差分导数，沿用 R=0.12 Ω、L=50 µH。
PWM、电流保护、电流环及运动控制参数保持原值。

本次直接编译实际 `music.c`、`foc.c`、`control.c` 和 `app.c` 为主机库，完整输出 737280 个控制周期，
逐样本核对打包解码、插值及前馈，并检查 CRLF 命令、播放时长、D/Q 音阶兼容、停止、重播、
未知歌曲、状态拒绝、故障中止、运动模式保留和剩余电流预算。
离线生日歌 Id 目标峰值为 19.53125 A，前馈峰值约 8.388 V。
Debug/Release 的 `MUSIC_ENABLE=0/1` 四种构建均通过；最终恢复为 1。
最终 Release 占用 Flash 593104 字节（75.42%），Debug 602760 字节（76.64%），RAM 均为 3648 字节。
这些结果验证软件输出，不能证明板上的 PWM 截止余量、实际声音、温升或旋转效果。

历史实机记录：[电流分析](20261004-current-audio-analysis.md)、
[采集时序修复](20261004-capture-timing-fix.md)、[整体代码审查](../20261004-code-audit.md)。
