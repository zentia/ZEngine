@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "ROOT_DIR=%%~fI"

set "CONFIG=Release"
set "ARCH=x64"
set "CLEAN=0"
set "COLLECT_SYMBOLS=1"
set "GENERATOR=Visual Studio 17 2022"
if not defined PARALLEL_JOBS set "PARALLEL_JOBS=%NUMBER_OF_PROCESSORS%"
if "%PARALLEL_JOBS%"=="" set "PARALLEL_JOBS=8"

:parse_args
if "%~1"=="" goto :main
if /I "%~1"=="--config" (
    set "CONFIG=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--arch" (
    set "ARCH=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--jobs" (
    set "PARALLEL_JOBS=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--generator" (
    set "GENERATOR=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--clean" (
    set "CLEAN=1"
    shift
    goto :parse_args
)
if /I "%~1"=="--no-symbols" (
    set "COLLECT_SYMBOLS=0"
    shift
    goto :parse_args
)
if /I "%~1"=="--help" goto :help
echo Unknown option: %~1
goto :usage

:help
echo Usage: %~nx0 [--arch x64] [--config Release] [--jobs N] [--generator "Visual Studio 17 2022"] [--clean] [--no-symbols]
echo Output:  bin\win64\ARCH\CONFIG\ZProfilerCsvExporter.exe
echo Symbols: bin\win64\symbols\CONFIG\ARCH\ZProfilerCsvExporter.*
exit /b 0

:usage
echo Usage: %~nx0 [--arch x64] [--config Release] [--jobs N] [--generator "Visual Studio 17 2022"] [--clean] [--no-symbols]
echo Output:  bin\win64\ARCH\CONFIG\ZProfilerCsvExporter.exe
echo Symbols: bin\win64\symbols\CONFIG\ARCH\ZProfilerCsvExporter.*
exit /b 1

:main
set "BUILD_DIR=%ROOT_DIR%\build\lib\win64\%ARCH%"
set "OUTPUT_SUBDIR=win64/%ARCH%"
set "OUT_DIR=%ROOT_DIR%\bin\win64\%ARCH%\%CONFIG%"
set "OUT_EXE=%OUT_DIR%\ZProfilerCsvExporter.exe"
set "OUT_PDB=%OUT_DIR%\ZProfilerCsvExporter.pdb"
set "OUT_ILK=%OUT_DIR%\ZProfilerCsvExporter.ilk"
set "SYMBOL_DIR=%ROOT_DIR%\bin\win64\symbols\%CONFIG%\%ARCH%"

echo ========================================
echo ZProfilerCsvExporter Win64 Build
echo ========================================
echo Root:      %ROOT_DIR%
echo Generator: %GENERATOR%
echo Arch:      %ARCH%
echo Config:    %CONFIG%
echo Output:    %OUT_EXE%
echo Symbols:   %SYMBOL_DIR%
echo Optimize:  ON
echo.

if "%CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -A "%ARCH%" ^
  -DZENGINE_OUTPUT_SUBDIR="%OUTPUT_SUBDIR%" ^
  -DZENGINE_WINDOWS_SIZE_OPTIMIZE=ON ^
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=OFF ^
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target ZProfilerCsvExporter --parallel %PARALLEL_JOBS%
if errorlevel 1 exit /b 1

if not exist "%OUT_EXE%" (
    echo [ERROR] Output executable not found: %OUT_EXE%
    exit /b 1
)

if "%COLLECT_SYMBOLS%"=="1" (
    if not exist "%SYMBOL_DIR%" mkdir "%SYMBOL_DIR%"
    if exist "%OUT_PDB%" move /Y "%OUT_PDB%" "%SYMBOL_DIR%\ZProfilerCsvExporter.pdb" >nul
    if exist "%OUT_ILK%" move /Y "%OUT_ILK%" "%SYMBOL_DIR%\ZProfilerCsvExporter.ilk" >nul
)

echo.
echo [SUCCESS] %OUT_EXE%
if "%COLLECT_SYMBOLS%"=="1" echo [SYMBOLS] %SYMBOL_DIR%
exit /b 0
