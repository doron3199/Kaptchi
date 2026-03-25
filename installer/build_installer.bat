@echo off
setlocal

echo ========================================
echo  Kaptchi Installer Build Script
echo ========================================
echo.

:: Step 1: Build Flutter release
echo [1/2] Building Flutter Windows release...
cd /d "%~dp0\.."
call flutter build windows --release
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Flutter build failed!
    pause
    exit /b 1
)
echo Flutter build complete.
echo.

:: Step 2: Build Inno Setup installer
echo [2/2] Building installer with Inno Setup...

:: Try common Inno Setup install locations
set ISCC=""
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
    set ISCC="C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
) else if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
    set ISCC="C:\Program Files\Inno Setup 6\ISCC.exe"
) else (
    :: Try PATH
    where ISCC.exe >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        set ISCC=ISCC.exe
    ) else (
        echo ERROR: Inno Setup 6 not found!
        echo Please install from https://jrsoftware.org/isdl.php
        pause
        exit /b 1
    )
)

%ISCC% "%~dp0\kaptchi_setup.iss"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Inno Setup compilation failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo  Build complete!
echo  Installer: installer\Output\KaptchiSetup.exe
echo ========================================
pause
