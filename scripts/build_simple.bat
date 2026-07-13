@echo off
chcp 65001 >nul
title DiskDelta - Simple MSVC Build

echo ============================================
echo  DiskDelta - 使用 MSVC 命令行编译
echo ============================================
echo.

REM Check for VS environment
where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cl.exe not found. Please run from VS Developer Command Prompt.
    echo   e.g.: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    pause
    exit /b 1
)

set PROJECT_DIR=%~dp0..
cd /d "%PROJECT_DIR%"

if not exist build mkdir build
cd build

set SRC=..\src\main.cpp ..\src\mft_reader.cpp ..\src\diff_engine.cpp ..\src\http_server.cpp
set LIBS=ws2_32.lib shell32.lib user32.lib

echo [INFO] Compiling...
cl /nologo /O2 /EHsc /utf-8 /W3 /D_WIN32_WINNT=0x0A00 /Fe:DiskDelta.exe %SRC% /link %LIBS%
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed!
    pause
    exit /b 1
)

echo.
echo [INFO] Copying web files...
if not exist web mkdir web
xcopy /E /Y /I ..\web .\web >nul 2>nul
if not exist data mkdir data

echo.
echo ============================================
echo  Build successful!
echo ============================================
echo.
echo  Output: %CD%\DiskDelta.exe
echo.
echo  To run (must be Administrator):
echo     DiskDelta.exe
echo.
echo  Or specify a different port:
echo     DiskDelta.exe --port 8080
echo.
echo  Open browser at: http://127.0.0.1:45678/
echo.

pause
