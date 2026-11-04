@echo off
chcp 65001 >nul
setlocal

rem ===========================================================================
rem  relay-win.bat —— 在 Windows 上启动 MQTT -> TLS 中转
rem
rem  开发板（GEC6818）没有 libssl、也没有外网，说不了 MQTTS。
rem  这个中转替它把 TLS 接上：
rem      板子 --明文 MQTT--> 172.20.120.100:1883 --TLS--> 华为云 8883
rem
rem  必须在 Windows 上跑，不能跑在 WSL 里：
rem      实测 WSL(mirrored) 里 bind 0.0.0.0:1883，Windows 自己能连，
rem      但板子从网线那头连不进来（被防火墙挡），换成 Windows 进程就正常。
rem
rem  用法：双击本文件，窗口别关。
rem ===========================================================================

set SCRIPT=%~dp0mqtts_relay.py
set LISTEN=172.20.120.100:1883

echo.
echo   MQTT -^> MQTTS 中转   监听 %LISTEN%
echo   脚本: %SCRIPT%
echo   关闭本窗口即停止中转。
echo.

where python >nul 2>&1
if %errorlevel%==0 (
    python "%SCRIPT%" --listen %LISTEN% -v
    goto :eof
)

rem 没在 PATH 里就试几个常见位置
for %%P in (
    "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
    "%LOCALAPPDATA%\Programs\Python\Python311\python.exe"
    "C:\Python312\python.exe"
    "C:\Python311\python.exe"
) do (
    if exist %%P (
        %%P "%SCRIPT%" --listen %LISTEN% -v
        goto :eof
    )
)

echo   [错误] 找不到 python。请先安装 Python 3，或把 python.exe 加进 PATH。
echo   也可以手动执行： python "%SCRIPT%" --listen %LISTEN% -v
pause
