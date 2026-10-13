@echo off
rem 启动 Python 前端（默认 http://0.0.0.0:5000）
cd /d "%~dp0..\frontend"
python app.py
