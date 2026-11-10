@echo off
rem 旧的一键启动流程：客户端带 DevTools 端口 + 本地配置服务 + 界面脚本监听注入。
rem 现在日常用 NexExclusive.exe 就够了，这个脚本只在调试界面脚本时用。
rem
rem 客户端路径不再写死：优先用环境变量 NEX_CLIENT，其次用 nexlaunch.exe 探测。
setlocal
chcp 65001 >nul
set "TOOLS=%~dp0"
set "UI=%TOOLS%..\ui"

if defined NEX_CLIENT goto :launch
echo 没设置 NEX_CLIENT 环境变量。
echo 请先：set NEX_CLIENT=D:\你的\cloudmusic.exe
pause
exit /b 2

:launch
start "" "%NEX_CLIENT%" --remote-debugging-port=9333
echo 等待客户端启动...
timeout /t 10 /nobreak >nul

start "nexctl" /min node "%TOOLS%nexctl.mjs" serve
timeout /t 2 /nobreak >nul
start "nexwatch" /min node "%TOOLS%nexctl.mjs" watch "%UI%\panel.js" "%UI%\simplify.js"

echo 已注入。设置页的「播放」里可以看到独占输出与界面精简。
timeout /t 3 /nobreak >nul
