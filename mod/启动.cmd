@echo off
title NeteaseExclusive - WASAPI 独占输出
cd /d "%~dp0"

echo.
echo   NeteaseExclusive
echo   ================
echo   正在启动客户端、注入音频模块、准备设置面板...
echo   就绪后本窗口可以最小化。
echo.
echo   关闭本窗口会停止看护；已经启动的客户端会继续播放。
echo.

"build-check\NexExclusive.exe"

echo.
echo   宿主已退出。
pause
