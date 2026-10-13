@echo off
rem 编译 C++ 后端（需要 MSYS2 MinGW64：pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-make）
setlocal
cd /d "%~dp0.."
if "%MINGW_HOME%"=="" set MINGW_HOME=C:\msys64\mingw64
set PATH=%MINGW_HOME%\bin;%PATH%
cmake -S backend -B backend\build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=%MINGW_HOME% -DOPENSSL_ROOT_DIR=%MINGW_HOME% -DCMAKE_CXX_COMPILER=%MINGW_HOME%\bin\g++.exe || exit /b 1
cmake --build backend\build -j 4 || exit /b 1
rem 复制运行所需 DLL，使可执行文件可独立运行
for %%d in (libssl-3-x64.dll libcrypto-3-x64.dll libsqlite3-0.dll libwinpthread-1.dll) do (
  if exist "%MINGW_HOME%\bin\%%d" copy /y "%MINGW_HOME%\bin\%%d" backend\build\ >nul
)
echo 编译完成: backend\build\shtx_backend.exe
