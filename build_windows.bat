@echo off
REM ZEngine Build Script for Windows
REM This script uses the unified Python build tool (zbuild.py)
REM Fixed: Set CMAKE_GIT_EXECUTABLE to avoid UGit version parsing issue

echo.
echo ========================================
echo ZEngine Build System
echo ========================================
echo.

REM Fix Git version detection issue
REM Get short path (8.3 format) to avoid spaces in path
set "GIT_EXE=C:\Program Files\Git\bin\git.exe"
set "GIT_SHORT="
for %%i in ("%GIT_EXE%") do set "GIT_SHORT=%%~si"
if exist "%GIT_EXE%" (
    echo [INFO] Using standard Git: %GIT_EXE%
    echo [INFO] Short path: %GIT_SHORT%
    REM Only the -D here. The literal "--extra-args" belongs to the configure
    REM line below and must appear exactly once -- see the note at :build.
    set "GIT_EXTRA=-DCMAKE_GIT_EXECUTABLE=%GIT_SHORT%"
) else (
    echo [WARNING] Standard Git not found at %GIT_EXE%
    echo [INFO] Please install Git for Windows from https://git-scm.com/
    set "GIT_EXTRA="
)

REM Check if Python is available
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python is not installed or not in PATH
    echo Please install Python 3.6 or later
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

REM Check if zbuild.py exists
if not exist "zbuild.py" (
    echo [ERROR] zbuild.py not found in current directory
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

REM Parse arguments
set CONFIG=debug
set TARGET=ZEditor
set JOBS=
REM Auto-detect CPU count for parallel builds (default)
REM Try to get CPU count using PowerShell (more reliable than wmic)
for /f %%I in ('powershell -Command "[System.Environment]::ProcessorCount"') do set CPU_COUNT=%%I
if "%CPU_COUNT%"=="" set CPU_COUNT=4
if "%JOBS%"=="" set JOBS=%CPU_COUNT%

:parse_args
if "%1"=="" goto :build
if "%1"=="release" set CONFIG=release
if "%1"=="debug" set CONFIG=debug
if "%1"=="--target" (
    set TARGET=%2
    shift
)
if "%1"=="--jobs" (
    set JOBS=%2
    shift
)
shift
goto :parse_args

:build
echo [INFO] Configuring build...
set CONFIGURE_EXTRA=
if defined GITHUB_ACTIONS (
    REM CI: editor smoke build uses DX12 only; runners do not ship VULKAN_SDK.
    set "CONFIGURE_EXTRA=--extra-args %GIT_EXTRA% -DZENGINE_USE_VULKAN=OFF"
) else (
    set "CONFIGURE_EXTRA=--extra-args %GIT_EXTRA%"
)
REM A release build is what gets packaged, so its editor defaults to a full
REM rebuild (ZENGINE_PACKAGING_FORCE_REBUILD): an incremental package is only
REM reproducible by whoever owns the cache. Still just a default -- the console
REM command `bundle.build --incremental` opts back in at run time.
REM --extra-args is argparse REMAINDER: it swallows *everything* after it, so
REM it may appear exactly ONCE and every -D has to be appended to that one
REM group. A second "--extra-args" is not a second group -- it is passed to
REM CMake verbatim, and CMake rejects it outright:
REM     CMake Error: Unknown argument --extra-args
REM which failed the configure step of every Windows CI run from b135346c on
REM (the flag was appended after the git one instead of joining it).
REM Passed explicitly both ways rather than only for release: CMake options are
REM sticky and the Windows presets are multi-config (one build dir for both),
REM so leaving it unset would let a previous release configure keep forcing
REM full rebuilds in a debug build.
if "%CONFIG%"=="release" (
    set "FORCE_REBUILD_ARG=-DZENGINE_PACKAGING_FORCE_REBUILD=ON"
) else (
    set "FORCE_REBUILD_ARG=-DZENGINE_PACKAGING_FORCE_REBUILD=OFF"
)
REM Same single --extra-args group: append, never start another one.
set "CONFIGURE_EXTRA=%CONFIGURE_EXTRA% %FORCE_REBUILD_ARG%"
python zbuild.py configure --config %CONFIG% %CONFIGURE_EXTRA%
if errorlevel 1 (
    echo [ERROR] Configuration failed
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

echo.
echo [INFO] Building project...
set BUILD_ARGS=build --config %CONFIG%
if not "%TARGET%"=="" set BUILD_ARGS=%BUILD_ARGS% --target %TARGET%
if not "%JOBS%"=="" set BUILD_ARGS=%BUILD_ARGS% --jobs %JOBS%

python zbuild.py %BUILD_ARGS%
if errorlevel 1 (
    echo [ERROR] Build failed
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

echo.
echo [SUCCESS] Build completed successfully!
echo.
if not defined GITHUB_ACTIONS pause
