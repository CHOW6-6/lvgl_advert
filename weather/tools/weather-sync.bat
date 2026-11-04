@echo off
rem ===========================================================================
rem  weather-sync.bat —— 给 Windows 计划任务用的一轮"抓天气 + 推板子"
rem
rem  计划任务每 5 分钟调它一次，它再进 WSL 跑 weather_watch.py --once。
rem  不要直接双击跑（会一闪而过）；双击也没坏处，只是看不到输出。
rem
rem  日志：WSL 里的 /tmp/weather_watch.log
rem ===========================================================================

C:\Windows\System32\wsl.exe -d Ubuntu -e bash -lc "cd /home/user/code/lv_port_pc_vscode-release-v8 && python3 weather/tools/weather_watch.py --once >> /tmp/weather_watch.log 2>&1"
