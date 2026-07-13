@echo off
chcp 65001 >nul
title DiskDelta - MinGW-w64 Build

echo ============================================
echo  DiskDelta - MinGW-w64 编译脚本
echo ============================================
echo.

REM Check for MinGW-w64 g++
where g++ >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] g++ not found in PATH.
    echo.
    echo Make sure MinGW-w64 is installed and in your PATH.
    echo   e.g.: D:\msys64\ucrt64\bin
    pause
    exit /b 1
)

set PROJECT_DIR=%~dp0..
cd /d "%PROJECT_DIR%"

REM Create build directory
if not exist build_mingw mkdir build_mingw

echo [INFO] Compiler: 
g++ --version | findstr /i "g++"

echo.
echo [INFO] Compiling DiskDelta with MinGW-w64...
echo.

g++ -std=c++17 -O2 -s ^
    -D_WIN32_WINNT=0x0A00 ^
    -I src ^
    src/main.cpp src/mft_reader.cpp src/diff_engine.cpp src/http_server.cpp ^
    -o build_mingw\DiskDelta.exe ^
    -lws2_32 -lshell32 -static

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Compilation failed!
    pause
    exit /b 1
)

echo.
echo [INFO] Copying web files...
if not exist build_mingw\web mkdir build_mingw\web
xcopy /E /Y /I web build_mingw\web >nul 2>nul
if not exist build_mingw\data mkdir build_mingw\data

echo.
echo ============================================
echo  Build successful!
echo ============================================
echo.
echo  Output: %CD%\build_mingw\DiskDelta.exe
echo  Size:   ~20 MB (statically linked)
echo.
echo  To run (must be Administrator):
echo     build_mingw\DiskDelta.exe
echo.
echo  Then open: http://127.0.0.1:45678/
echo.
pause
