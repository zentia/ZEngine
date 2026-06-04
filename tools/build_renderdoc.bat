@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Build vendored RenderDoc via CMake wrapper (tools/renderdoc_build/cmake).
REM CMake selects the local VS 2022 / MSBuild toolset; MSBuild drives renderdoc.sln
REM without opening the solution in Visual Studio (no upgrade / redirect prompts).
REM
REM Usage:
REM   tools\build_renderdoc.bat                  Development x64 (default)
REM   tools\build_renderdoc.bat --release        Release x64
REM   tools\build_renderdoc.bat --reconfigure    Force cmake configure
REM   tools\build_renderdoc.bat --clean          Remove cmake build dirs
REM   tools\build_renderdoc.bat --fetch-3rdparty Download qrenderdoc_3rdparty.zip
REM
REM Outputs (typical):
REM   tools\renderdoc\x64\Development\renderdoc.dll
REM   tools\renderdoc\x64\Development\qrenderdoc.exe

set "PRESET=windows-dev"
set "CONFIG=Development"
set "DO_CONFIGURE=1"
set "DO_BUILD=1"
set "DO_CLEAN=0"
set "FETCH_3RDPARTY=0"

REM Save before parse_args: shift moves %0 as well, so %~dp0 is wrong after the loop.
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "TOOLS_DIR=%%~fI"
set "RENDERDOC_ROOT=%TOOLS_DIR%\renderdoc"
set "RENDERDOC_BUILD_DIR=%TOOLS_DIR%\renderdoc_build"
set "CMAKE_DIR=%RENDERDOC_BUILD_DIR%\cmake"

:parse_args
if "%~1"=="" goto end_parse
if /i "%~1"=="--release" (
    set "PRESET=windows-release"
    set "CONFIG=Release"
    shift
    goto parse_args
)
if /i "%~1"=="--development" (
    set "PRESET=windows-dev"
    set "CONFIG=Development"
    shift
    goto parse_args
)
if /i "%~1"=="--reconfigure" (
    set "DO_CONFIGURE=1"
    shift
    goto parse_args
)
if /i "%~1"=="--no-configure" (
    set "DO_CONFIGURE=0"
    shift
    goto parse_args
)
if /i "%~1"=="--clean" (
    set "DO_CLEAN=1"
    shift
    goto parse_args
)
if /i "%~1"=="--rebuild" (
    set "DO_CONFIGURE=1"
    set "DO_BUILD=1"
    set "CLEAN_FIRST=1"
    shift
    goto parse_args
)
if /i "%~1"=="--fetch-3rdparty" (
    set "FETCH_3RDPARTY=1"
    shift
    goto parse_args
)
if /i "%~1"=="--help" goto show_help
if /i "%~1"=="-h" goto show_help
if /i "%~1"=="/?" goto show_help
echo [ERROR] Unknown argument: %~1
goto show_help
:end_parse

if not exist "%CMAKE_DIR%\CMakeLists.txt" (
    echo [ERROR] CMake wrapper not found: %CMAKE_DIR%\CMakeLists.txt
    exit /b 1
)
if not exist "%RENDERDOC_ROOT%\renderdoc.sln" (
    echo [ERROR] renderdoc.sln not found under "%RENDERDOC_ROOT%"
    exit /b 1
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found on PATH
    exit /b 1
)

if "%DO_CLEAN%"=="1" (
    echo [INFO] Removing CMake build dirs under tools\renderdoc_build\build\
    if exist "%RENDERDOC_BUILD_DIR%\build\cmake-dev" rmdir /s /q "%RENDERDOC_BUILD_DIR%\build\cmake-dev"
    if exist "%RENDERDOC_BUILD_DIR%\build\cmake-release" rmdir /s /q "%RENDERDOC_BUILD_DIR%\build\cmake-release"
)

if "%FETCH_3RDPARTY%"=="1" (
    call :fetch_qrenderdoc_3rdparty
    if errorlevel 1 exit /b 1
)

for /f %%I in ('powershell -NoProfile -Command "[System.Environment]::ProcessorCount"') do set "CPU_COUNT=%%I"
if not defined CPU_COUNT set "CPU_COUNT=4"

echo.
echo ========================================
echo RenderDoc build (CMake + MSBuild)
echo ========================================
echo [INFO] Preset:       %PRESET%
echo [INFO] Configuration: %CONFIG%
echo [INFO] CMake dir:    %CMAKE_DIR%
echo [INFO] Parallel:     %CPU_COUNT%
echo.

pushd "%CMAKE_DIR%"

if "%DO_CONFIGURE%"=="1" (
    cmake --preset %PRESET%
    if errorlevel 1 (
        echo [ERROR] cmake configure failed
        popd
        exit /b 1
    )
)

if "%DO_BUILD%"=="1" (
    if defined CLEAN_FIRST (
        cmake --build --preset %PRESET% --target renderdoc --parallel %CPU_COUNT% --clean-first
    ) else (
        cmake --build --preset %PRESET% --target renderdoc --parallel %CPU_COUNT%
    )
    if errorlevel 1 (
        echo [ERROR] cmake build failed
        popd
        exit /b 1
    )
)

popd

set "OUT_DIR=%RENDERDOC_ROOT%\x64\%CONFIG%"
echo.
echo [OK] RenderDoc build succeeded.
echo [INFO] Output folder: %OUT_DIR%
if exist "%OUT_DIR%\renderdoc.dll" echo [INFO]   renderdoc.dll
if exist "%OUT_DIR%\qrenderdoc.exe" echo [INFO]   qrenderdoc.exe
if exist "%OUT_DIR%\renderdoccmd.exe" echo [INFO]   renderdoccmd.exe
echo.
exit /b 0

:fetch_qrenderdoc_3rdparty
set "ZIP_URL=https://renderdoc.org/qrenderdoc_3rdparty.zip"
set "ZIP_FILE=%RENDERDOC_ROOT%\qrenderdoc_3rdparty.zip"
echo [INFO] Fetching optional qrenderdoc extras from %ZIP_URL%
curl.exe -fL "%ZIP_URL%" -o "%ZIP_FILE%"
if errorlevel 1 (
    echo [ERROR] curl failed to download qrenderdoc_3rdparty.zip
    exit /b 1
)
powershell -NoProfile -Command "Expand-Archive -LiteralPath '%ZIP_FILE%' -DestinationPath '%RENDERDOC_ROOT%' -Force"
if errorlevel 1 (
    echo [ERROR] Failed to extract qrenderdoc_3rdparty.zip
    exit /b 1
)
echo [INFO] qrenderdoc_3rdparty extracted into %RENDERDOC_ROOT%
exit /b 0

:show_help
echo.
echo Usage: tools\build_renderdoc.bat [options]
echo.
echo   --development     Development build (default preset windows-dev)
echo   --release         Release build (preset windows-release)
echo   --reconfigure     Run cmake configure even if cache exists
echo   --no-configure    Skip configure, build only
echo   --rebuild         Configure + clean-first build
echo   --clean           Delete tools\renderdoc_build\build\cmake-* dirs
echo   --fetch-3rdparty  Download qrenderdoc_3rdparty.zip before build
echo   --help            Show this help
echo.
echo CMake wrapper: tools\renderdoc_build\cmake\  (presets in CMakePresets.json)
echo.
exit /b 1
