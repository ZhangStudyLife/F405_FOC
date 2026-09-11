@echo off
rem 双击这个文件即可打开波形上位机（不需要串口，走 ST-Link/SWD）。
rem 前提：板子已上电、ST-Link 已插好、固件已烧录。
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8
python "%~dp0scope_gui.py" %*
if errorlevel 1 (
    echo.
    echo 启动失败，常见原因：
    echo   1. 没装 Python 或 python 不在 PATH 里
    echo   2. ST-Link 没插好 / 板子没上电
    echo   3. 固件没烧录（先跑 foc.sh build Release 和 foc.sh flash Release）
    echo.
    pause
)
