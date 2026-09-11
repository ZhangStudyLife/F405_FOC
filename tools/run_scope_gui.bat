@echo off
rem ===================================================================
rem  MT6835 position/speed waveform GUI  (over ST-Link / SWD)
rem
rem  NOTE: keep this file PURE ASCII.
rem  cmd.exe parses .bat files using the system ANSI codepage (GBK on
rem  Chinese Windows), so UTF-8 Chinese text here turns into mojibake
rem  and cmd tries to execute the garbage as commands.
rem ===================================================================

cd /d "%~dp0.."

rem Do NOT set PYTHONIOENCODING=utf-8 here: the console is cp936 on
rem Chinese Windows, so forcing UTF-8 output would turn every Chinese
rem message into mojibake. Python already writes correctly to a real
rem console (it uses WriteConsoleW since 3.6), so leave it alone.

where python >nul 2>nul
if errorlevel 1 (
    echo.
    echo   [ERROR] "python" not found on PATH.
    echo   Install Python 3 and make sure it is added to PATH.
    echo.
    pause
    exit /b 1
)

python "%~dp0scope_gui.py" %*

if errorlevel 1 (
    echo.
    echo   [ERROR] GUI failed to start. Common causes:
    echo     1. ST-Link not connected, or board not powered
    echo     2. Firmware not flashed yet
    echo        run:  foc.sh build Release
    echo              foc.sh flash Release
    echo     3. Another OpenOCD / debugger session is holding the ST-Link
    echo.
    pause
)
