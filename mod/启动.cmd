@echo off
title NexExclusive
chcp 65001 >nul
cd /d "%~dp0"

rem 找宿主：优先 build-check（开发构建），其次脚本所在目录（发布包结构）。
set "HOST="
if exist "build-check\NexExclusive.exe" set "HOST=build-check\NexExclusive.exe"
if not defined HOST if exist "NexExclusive.exe" set "HOST=NexExclusive.exe"

if not defined HOST (
    echo 没找到 NexExclusive.exe。
    echo 先构建：bash build.sh all
    pause
    exit /b 2
)

echo.
echo   NexExclusive
echo   ================
echo   启动客户端、注入音频模块、准备配置服务...
echo   之后本窗口可以最小化，程序会驻留在右下角托盘。
echo.
echo   关闭本窗口或从托盘退出，已经启动的客户端会继续留着。
echo.

"%HOST%"

echo.
echo   宿主已退出。
pause
