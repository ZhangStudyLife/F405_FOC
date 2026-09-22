@echo off
rem ============================================================
rem  405_FOC  PC bench entry point (motor test automation)
rem  Double-click  = run the case library on all four groups
rem  Terminal use  = bench.cmd list | discover
rem                  bench.cmd run [--mode torque|speed|position|all]
rem                               [--dry-run] [--yes] [--groups 0,1,2,3]
rem                  bench.cmd report <run-directory>
rem                  bench.cmd chain  <run-directory> <case>
rem
rem  NOTE: keep this file pure ASCII. cmd.exe parses .cmd files
rem  using the OEM codepage (GBK on zh-CN), so non-ASCII comments
rem  get garbled and can break the script body.
rem ============================================================
chcp 65001 >nul
set PYTHONUTF8=1
if "%~1"=="" (
    python "%~dp0..\tools\bench\bench.py" run
) else (
    python "%~dp0..\tools\bench\bench.py" %*
)
set "result=%errorlevel%"
if not "%result%"=="0" pause
if "%~1"=="" pause
exit /b %result%
