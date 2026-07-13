@echo off
chcp 65001 >nul
title DiskDelta Build Script

echo ============================================
echo  DiskDelta - NTFS MFT Scanner ^& Diff Tool
echo  Build Script
echo ============================================
echo.

REM Check for VS environment
where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Visual Studio compiler (cl.exe) not found.
    echo.
    echo Please run this script from a Visual Studio Developer Command Prompt:
    echo   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    echo.
    echo Or open "Developer Command Prompt for VS 2022" from Start Menu.
    pause
    exit /b 1
)

REM Check for CMake
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake not found. Please install CMake from https://cmake.org/
    pause
    exit /b 1
)

REM Get project root
set PROJECT_DIR=%~dp0..
cd /d "%PROJECT_DIR%"

echo [INFO] Project root: %CD%
echo [INFO] Creating build directory...
if not exist build mkdir build
cd build

echo [INFO] Configuring with CMake...
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [WARN] Ninja not available, trying Visual Studio 17 2022...
    cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
)
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [WARN] Trying default generator...
    cmake .. -DCMAKE_BUILD_TYPE=Release
)
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed!
    pause
    exit /b 1
)

echo [INFO] Building...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ============================================
echo  Build successful!
echo ============================================
echo.
echo  Output: %CD%\bin\Release\DiskDelta.exe
echo  (or %CD%\bin\DiskDelta.exe)
echo.
echo  To run:
echo     .\bin\Release\DiskDelta.exe
echo.
echo  Or as administrator (recommended):
echo     runas /user:Administrator ".\bin\Release\DiskDelta.exe"
echo.

pause
