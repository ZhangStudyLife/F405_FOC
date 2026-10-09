# 电机歌曲：Epi / birthdaySong

默认固件播放用户提供的钢琴演奏《Epilogue》**0～60 秒**，命令为 `Music Epi`。
直接量化录音波形，保留旋律、和弦、强弱和衰减，不提取为单音音符，也不改变速度或音高。
当前 Epi 使用已处理音频：80 Hz 二阶高通、300 Hz 低架 −4.5 dB、1600 Hz 宽带 +3.5 dB，
以及 2:1 动态压缩（10 ms attack、150 ms release），随后量化。离线综合响度约 −17.2 LUFS。
音源文件名和 SHA256 保存在 [Epi.inc](Epi.inc) 开头。
[birthdaySong.inc](birthdaySong.inc) 保留原来的完整 36.864 秒生日歌数据。

## 编译选择

修改 [../Config/music_config.h](../Config/music_config.h)：

```c
#define MUSIC_ENABLE 1
#define MUSIC_SELECTED_SONG MUSIC_SONG_EPI
```

将 `MUSIC_SELECTED_SONG` 改为 `MUSIC_SONG_BIRTHDAY` 可切换生日歌。
预处理只包含所选歌曲的 `.inc`，每份固件最多一首，不需要删除本地歌曲文件。
`MUSIC_ENABLE=0` 排除歌曲数据及播放实现，拒绝播放命令。

| 选择 | 命令 | 采样率 / 位深 | 时长 | 歌曲 Flash 数据 |
|---|---|---|---|---|
| `MUSIC_SONG_EPI`（默认） | `Music Epi` | 8000 Hz / PCM12 | 60 秒 | 720009 字节 |
| `MUSIC_SONG_BIRTHDAY` | `Music birthdaySong` | 10000 Hz / PCM12 | 36.864 秒 | 552969 字节 |

程序区容量为 768 KiB；0x080C0000～0x080FFFFF 仍用于参数与校准。
歌曲数组位于 `.rodata`，直接读 Flash，不将整首歌曲搬入 RAM。
当前 Epi 构建：Release 占 769312 字节，剩余 17120 字节；Debug 占 785152 字节，剩余 1280 字节。
Epi 的 Release/Debug、生日歌 Release、关闭音乐的 Release 均已构建通过，符号表确认每份固件最多一首歌曲数据。

## 命令行为

默认 USB CDC；UART 开关、唯一命令来源及 JustFloat 回传见 [../README.md](../README.md)。
文本以 CR 或 LF 结束，CRLF 只执行一次；接受 `Music` 和 `music`，歌曲名区分大小写。

| 命令 | 行为 |
|---|---|
| `Music Epi` | 选中 Epi 时，D 轴从头播放一次；重复发送从头重播 |
| `Music birthdaySong` | 选中生日歌时，D 轴从头播放一次 |
| 未选中的上述歌曲命令 | 直接跳过，不启动、不重播、不停止已有音乐，不改变运动目标或功率状态 |
| `Music stop` / `music stop` | 停止音乐，清除音乐 Id/Iq，保留运动目标与功率状态 |
| `music play` | C4～C5 八音阶，4.8 秒 |
| `music play q` | 2 A 峰值 Q 轴音阶对照，仅接受零转矩且转速低于 5 rpm |
| `stop` | 关闭功率，清除音乐和运动目标 |

播放所选歌曲要求零偏、电角度校准有效，状态为 IDLE 或 RUN。
IDLE 时进入原有预充电并以 Iq=0 启动；RUN 时保留原速度、位置或转矩目标。
歌曲结束只将音乐目标归零，FOC 继续 RUN；关闭功率须发送 `stop`。
故障、停止、重新初始化和开始校准会终止播放。

JustFloat 通道 16 为命令计数，17 为结果：1 成功（含已知但未选中歌曲的跳过），
2 未知歌曲或语法错误，3 状态不允许。未知歌曲也不会打断已有音乐。

## 量化与播放

对源文件前 60 秒的 48 kHz 单声道解码进行 Welch 频谱分析，约 **99.940% 能量在 4 kHz 以下**。
原来的 10 kHz / PCM12 存 60 秒需 900009 字节，超过程序区。
Epi 使用 8 kHz / PCM12，混合单声道、抗混叠重采样、去直流、整体归一化，首尾各 20 ms 渐变。
这保留了主要钢琴频段，舍弃 4 kHz 以上信息和立体声空间感。

两样本打包三字节，对两个 12 位二进制补码整数 `a`、`b`（取 `& 0xfff`）：

```text
byte[0] = a & 0xff
byte[1] = (a >> 8) | ((b & 0x0f) << 4)
byte[2] = b >> 4
```

每首 `.inc` 包含 `uint8_t` 数组和 `{name, data, samples}` 描述。
`samples` 是有效样本数，前置两个零样本、尾部至少四个零样本，不计入时长。
生日歌维持 10 kHz 四点半采样插值；Epi 通过五相四点 Lagrange 插值到 20 kHz 电流环。
转换时检查各插值相位峰值，归一化到约 2000/2048，保留插值余量。
当前 Epi 的 20 kHz 插值峰值约 0.9692，播放共 1200000 个电流环周期。

每个 50 µs 周期输出 D 轴电流目标及 R/L 电压前馈，峰值沿用
`sqrt(max(0, 20² − Iq_motion²))` A；运动 Iq 不因音乐降低。
到 ±20 A 时音乐幅值归零，播放时间轴仍继续。
前馈使用下一 PWM 中心波形及中心差分导数，R=0.12 Ω、L=50 µH。

电机声音还取决于电流跟随与机械频响。当前完成音频转换和软件构建；
实际音色、运行时序及温升需要实机验证。

## 重新生成与构建

电脑需要 FFmpeg、Python 和 NumPy。在工程根目录执行：

```powershell
New-Item -ItemType Directory -Force build/audio_analysis | Out-Null
ffmpeg -v error -y -i "D:\Downloads\「Epilogue」lalaland爱乐之城--MappleZS钢琴演奏 - 1.爱乐之城(Av840715625,P1).mp3" -t 60 -ac 1 -ar 48000 -af "highpass=f=80:p=2,bass=g=-4.5:f=300:t=q:w=0.7,equalizer=f=1600:t=o:w=2:g=3.5,acompressor=threshold=0.12:ratio=2:attack=10:release=150:makeup=1" -c:a pcm_f32le build/audio_analysis/Epi_processed_48k.wav
python App/Music/convert_song.py build/audio_analysis/Epi_processed_48k.wav Epi --seconds 60 --rate 8000
cmake --preset Release
cmake --build --preset Release
python ../download/flash.py flash Release --erase=program
```

不带 `--seconds` 时转换完整音源，默认采样率为 10000 Hz，兼容生日歌转换方式。
采样率须与所选歌曲在配置头文件中的 `MUSIC_SAMPLE_RATE` 一致。
下载使用 `--erase=program` 保留参数和校准区。
