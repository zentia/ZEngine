@echo off
REM ZEngine Generate Script for Windows
REM This script generates the project files using CMake presets

REM Parse command line arguments
REM PAPI_TYPE selects the JS scripting backend. ZEngine is JS-only;
REM supported values: v8 (default on desktop/console), quickjs (mobile/web).
set PAPI_TYPE=v8
set USE_UNITY_BUILD=OFF
set PRESET_NAME=windows_visual_studio
:parse_args
if "%~1"=="" goto end_parse
if /i "%~1"=="--papi" (
    set PAPI_TYPE=%~2
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--unity" (
    set USE_UNITY_BUILD=ON
    shift
    goto parse_args
)
if /i "%~1"=="--fast" (
    set PRESET_NAME=windows_visual_studio_fast
    shift
    goto parse_args
)
if /i "%~1"=="--ninja" (
    set PRESET_NAME=windows_ninja
    shift
    goto parse_args
)
shift
goto parse_args
:end_parse

REM Validate PAPI_TYPE -- JS-only (quickjs / v8). Lua / Python / Node.js
REM backends were removed; ZEngine commits to TypeScript-first scripting
REM with a two-VM strategy (quickjs for JIT-restricted platforms, v8 for
REM desktop/console). See AGENTS.md 2.7.
if /i not "%PAPI_TYPE%"=="quickjs" if /i not "%PAPI_TYPE%"=="v8" (
    echo [ERROR] Invalid PAPI type: %PAPI_TYPE%
    echo [INFO] Valid options: quickjs, v8
    pause
    exit /b 1
)

echo.
echo ========================================
echo ZEngine Project Generation
echo ========================================
echo.
echo [INFO] PAPI Type: %PAPI_TYPE%
echo [INFO] Preset: %PRESET_NAME%
if "%USE_UNITY_BUILD%"=="ON" (
    echo [INFO] Unity Build: Enabled
) else (
    echo [INFO] Unity Build: Disabled
)
echo.
REM Get CPU count for display
for /f %%I in ('powershell -Command "[System.Environment]::ProcessorCount"') do set CPU_COUNT=%%I
if "%CPU_COUNT%"=="" set CPU_COUNT=4

echo [TIP] For faster builds and better CPU utilization:
echo.
echo   === Visual Studio IDE Builds ===
echo   1. Tools -^> Options -^> Projects and Solutions -^> Build and Run
echo      - Set "Maximum number of parallel project builds" to %CPU_COUNT% (your CPU core count)
echo      - Uncheck "Run build at low process priority" for faster builds
echo      - Uncheck "Only build startup projects and dependencies on Run"
echo.
echo   === Command Line Builds (RECOMMENDED for speed) ===
echo   2. Use parallel builds: cmake --build build --parallel %CPU_COUNT%
echo      Or: cmake --build build -j %CPU_COUNT%
echo.
echo   === Best Performance: Use Ninja Generator ===
echo   3. Use --ninja flag: gen_windows.bat --ninja
echo      Then build: cmake --build build/build_win_ninja --parallel %CPU_COUNT%
echo      Ninja is typically 2-3x faster than Visual Studio generator!
echo.
echo   === Other Options ===
echo   4. Use --fast flag to enable Unity Builds (faster full builds, slower incremental)
echo   5. Example: gen_windows.bat --ninja --papi lua
echo.

REM Check if running in VSCode/Cursor environment
REM Temporarily unset VSCODE_NLS_CONFIG to allow windows_visual_studio preset
set OLD_VSCODE_NLS_CONFIG=%VSCODE_NLS_CONFIG%
if defined VSCODE_NLS_CONFIG (
    echo [INFO] Detected VSCode/Cursor environment, temporarily unsetting VSCODE_NLS_CONFIG
    set VSCODE_NLS_CONFIG=
)

REM Check if Python is available (for unified tool)
python --version >nul 2>&1
if errorlevel 1 (
    echo [WARNING] Python not found, using CMake directly
    if "%USE_UNITY_BUILD%"=="ON" (
        cmake --preset "%PRESET_NAME%" -DPAPI_TYPE=%PAPI_TYPE% -DUSE_UNITY_BUILD=ON
    ) else (
        cmake --preset "%PRESET_NAME%" -DPAPI_TYPE=%PAPI_TYPE%
    )
) else (
    echo [INFO] Using unified build tool...
    if "%USE_UNITY_BUILD%"=="ON" (
        python zbuild.py configure --preset %PRESET_NAME% --extra-args "-DPAPI_TYPE=%PAPI_TYPE% -DUSE_UNITY_BUILD=ON"
    ) else (
        python zbuild.py configure --preset %PRESET_NAME% --extra-args "-DPAPI_TYPE=%PAPI_TYPE%"
    )
)

REM Restore VSCODE_NLS_CONFIG
if defined OLD_VSCODE_NLS_CONFIG (
    set VSCODE_NLS_CONFIG=%OLD_VSCODE_NLS_CONFIG%
)

if errorlevel 1 (
    echo [ERROR] Generation failed
    pause
    exit /b 1
)

echo.
echo [SUCCESS] Project files generated successfully!
echo.
pause