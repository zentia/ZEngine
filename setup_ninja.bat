@echo off
REM ZEngine Ninja Setup Script
REM This script sets up Ninja build system for maximum performance

echo.
echo ========================================
echo ZEngine Ninja Build Setup
echo ========================================
echo.

REM Check if Ninja is available
where ninja >nul 2>&1
if errorlevel 1 (
    echo [WARNING] Ninja not found in PATH
    echo.
    echo [INFO] Installing Ninja:
    echo   1. Download from: https://github.com/ninja-build/ninja/releases
    echo   2. Or install via: choco install ninja
    echo   3. Or install via: pip install ninja
    echo.
    echo [INFO] After installing, add ninja.exe to your PATH
    echo.
    pause
    exit /b 1
)

echo [INFO] Ninja found: 
ninja --version
echo.

REM Get CPU count
for /f %%I in ('powershell -Command "[System.Environment]::ProcessorCount"') do set CPU_COUNT=%%I
if "%CPU_COUNT%"=="" set CPU_COUNT=4

echo [INFO] Detected %CPU_COUNT% CPU cores
echo.

REM Parse arguments. PAPI_TYPE: quickjs (default) | v8. JS-only.
set PAPI_TYPE=quickjs
set USE_UNITY=OFF

:parse_args
if "%~1"=="" goto configure
if /i "%~1"=="--papi" (
    set PAPI_TYPE=%~2
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--unity" (
    set USE_UNITY=ON
    shift
    goto parse_args
)
if /i "%~1"=="--fast" (
    set USE_UNITY=ON
    shift
    goto parse_args
)
shift
goto parse_args

:configure
echo [INFO] Configuring Ninja build...
echo [INFO] PAPI Type: %PAPI_TYPE%
if "%USE_UNITY%"=="ON" (
    echo [INFO] Unity Build: Enabled
) else (
    echo [INFO] Unity Build: Disabled
)
echo.

REM Check if old build directory exists and suggest cleanup
if exist "build\build_win_ninja" (
    echo [WARNING] Old Ninja build directory found: build\build_win_ninja
    echo [INFO] If you encounter configuration errors, try cleaning first:
    echo    rmdir /s /q build\build_win_ninja
    echo.
    choice /C YN /M "Clean old build directory before configuring"
    if errorlevel 2 goto skip_clean
    if errorlevel 1 (
        echo [INFO] Cleaning old build directory...
        rmdir /s /q build\build_win_ninja 2>nul
        echo [INFO] Cleanup complete.
        echo.
    )
)
:skip_clean

REM Generate project files
if "%USE_UNITY%"=="ON" (
    call gen_windows.bat --ninja --papi %PAPI_TYPE% --unity
) else (
    call gen_windows.bat --ninja --papi %PAPI_TYPE%
)

if errorlevel 1 (
    echo [ERROR] Configuration failed
    pause
    exit /b 1
)

echo.
echo [SUCCESS] Ninja build system configured!
echo.
echo [INFO] To build, use one of these methods:
echo.
echo   1. Quick build script (recommended):
echo      build_fast.bat --ninja
echo.
echo   2. Manual build:
echo      cmake --build build/build_win_ninja --config RelWithDebInfo --parallel %CPU_COUNT%
echo.
echo   3. Build specific target:
echo      build_fast.bat --ninja --target ZEditor
echo.
echo   4. Build Debug:
echo      build_fast.bat --ninja --debug
echo.
pause
