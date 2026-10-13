@echo off
rem 一键启动：后端 + 前端（各开一个窗口）
start "SHTX Backend (C++)" cmd /k "%~dp0start_backend.bat"
timeout /t 2 /nobreak >nul
start "SHTX Frontend (Python)" cmd /k "%~dp0start_frontend.bat"
echo 浏览器访问 http://127.0.0.1:5000
