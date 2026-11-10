@echo off
rem 一键启动：客户端带 DevTools 端口，本地配置服务，界面脚本监听注入。
rem 客户端没开时先开；已开着但没带端口时需要先退出再运行本脚本。
setlocal
set "TOOLS=%~dp0"
set "UI=%TOOLS%..\ui"

start "" "D:\Installed_Program\CloudMusic\cloudmusic.exe" --remote-debugging-port=9333
echo 等待客户端启动...
timeout /t 10 /nobreak >nul

start "nexctl" /min node "%TOOLS%nexctl.mjs" serve
timeout /t 2 /nobreak >nul
start "nexwatch" /min node "%TOOLS%nexctl.mjs" watch "%UI%\panel.js" "%UI%\simplify.js"

echo 已注入。设置页的「播放」里可以看到独占输出与界面精简。
timeout /t 3 /nobreak >nul
