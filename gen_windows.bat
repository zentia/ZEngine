@echo off
REM ZEngine Generate Script for Windows
REM This script generates the project files using CMake presets

REM Parse command line arguments
REM PAPI_TYPE selects the JS scripting backend. ZEngine is JS-only;
REM supported values: v8 (release/relwithdebinfo), quickjs (debug/mobile/web).
REM Default is quickjs (debug builds use QuickJS for faster iteration).
REM USE_VULKAN controls -DZENGINE_USE_VULKAN. Default ON (Windows builds both
REM the DX12 and Vulkan backends); pass --no-vulkan for a DX12-only build that
REM needs no Vulkan SDK at all.
set PAPI_TYPE=quickjs
set USE_UNITY_BUILD=OFF
set USE_VULKAN=ON
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
REM --no-vulkan: DX12-only build. Skips the Vulkan SDK requirement entirely
REM (no VULKAN_SDK env var needed) and excludes every Vulkan translation unit
REM from ZRuntime / ShaderLab. See engine/CMakeLists.txt (ZENGINE_USE_VULKAN).
if /i "%~1"=="--no-vulkan" (
    set USE_VULKAN=OFF
    shift
    goto parse_args
)
REM --vulkan: explicit inverse of --no-vulkan (this is already the default).
if /i "%~1"=="--vulkan" (
    set USE_VULKAN=ON
    shift
    goto parse_args
)
if /i "%~1"=="--help" (
    goto usage
)
if /i "%~1"=="/?" (
    goto usage
)
shift
goto parse_args
:end_parse

REM Assemble the CMake -D list. Kept as a flat variable so it can be passed
REM through unquoted -- zbuild.py's --extra-args uses argparse REMAINDER, and a
REM single quoted "a b" string would arrive as ONE CMake argument.
set CMAKE_EXTRA_ARGS=-DPAPI_TYPE=%PAPI_TYPE%
if "%USE_UNITY_BUILD%"=="ON" set CMAKE_EXTRA_ARGS=%CMAKE_EXTRA_ARGS% -DUSE_UNITY_BUILD=ON
if "%USE_VULKAN%"=="OFF" set CMAKE_EXTRA_ARGS=%CMAKE_EXTRA_ARGS% -DZENGINE_USE_VULKAN=OFF

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
if "%USE_VULKAN%"=="OFF" (
    echo [INFO] Vulkan: Disabled (DX12 only, no Vulkan SDK required)
) else (
    echo [INFO] Vulkan: Enabled (DX12 default runtime + Vulkan switchable backend)
)
echo [INFO] CMake args: %CMAKE_EXTRA_ARGS%
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
echo   5. Use --no-vulkan flag for a DX12-only build (no Vulkan SDK needed):
echo      gen_windows.bat --no-vulkan
echo   6. Example: gen_windows.bat --ninja --papi v8 --no-vulkan
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
    cmake --preset "%PRESET_NAME%" %CMAKE_EXTRA_ARGS%
) else (
    echo [INFO] Using unified build tool...
    python zbuild.py configure --preset %PRESET_NAME% --extra-args %CMAKE_EXTRA_ARGS%
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
exit /b 0

:usage
echo.
echo ========================================
echo ZEngine Project Generation - Usage
echo ========================================
echo.
echo   gen_windows.bat [options]
echo.
echo Options:
echo   --papi ^<quickjs^|v8^>   JS scripting backend (default: quickjs)
echo   --unity              Enable Unity Build (faster full builds)
echo   --fast               Use the windows_visual_studio_fast preset (Unity Build on)
echo   --ninja              Use the windows_ninja preset (Ninja Multi-Config)
echo   --no-vulkan          DX12-only build: sets -DZENGINE_USE_VULKAN=OFF, so no
echo                        Vulkan SDK / VULKAN_SDK env var is required
echo   --vulkan             Keep Vulkan enabled (default)
echo   --help               Show this help
echo.
echo Examples:
echo   gen_windows.bat
echo   gen_windows.bat --ninja --papi v8
echo   gen_windows.bat --no-vulkan
echo   gen_windows.bat --ninja --fast --no-vulkan
echo.
pause
exit /b 0