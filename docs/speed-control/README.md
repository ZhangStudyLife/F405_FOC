# FOC 算法前后对照网页

直接打开 `index.html`，或在本目录运行 `python -m http.server 8777 --bind 127.0.0.1`。
图像已经内嵌，网页正文离线可读；各图提供 SVG、PDF、PNG 下载。

`render.py` 使用 NumPy、pandas、SciPy、Matplotlib 从工程原始记录生成图表、绘图 CSV、
指标与原始来源哈希。所需原始目录为 `build/bench_debug/20261003_030219/`、
`20261003_040347/`、`20261003_044515/`，具体文件见 `data/provenance.json`。
`build_page.py` 生成网页和下载包；下载包含图用数据，完整原始日志保留在上述工程目录。

复现命令：

```powershell
python render.py
python build_page.py
```

实测与名义模型必须分开解释。本轮高速六点为单次短测，低速发现退化，整体未验收通过。
最终板上恢复 2dc6b77 正式固件，参数/校准区逐字节不变，功率关闭。
