@echo off
rem 启动 C++ 后端（默认 http://127.0.0.1:8080）
cd /d "%~dp0.."
if "%MINGW_HOME%"=="" set MINGW_HOME=C:\msys64\mingw64
set PATH=%~dp0..\backend\build;%MINGW_HOME%\bin;%PATH%
backend\build\shtx_backend.exe config.json
