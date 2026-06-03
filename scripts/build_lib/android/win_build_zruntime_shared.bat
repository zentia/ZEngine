@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "ROOT_DIR=%%~fI"

set "CONFIG=Release"
set "ABI=arm64-v8a"
set "ANDROID_PLATFORM=android-24"
set "CLEAN=0"
set "STRIP_OUTPUT=1"
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
if /I "%~1"=="--abi" (
    set "ABI=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--platform" (
    set "ANDROID_PLATFORM=%~2"
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
if /I "%~1"=="--ndk" (
    set "ANDROID_NDK_ROOT=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--clean" (
    set "CLEAN=1"
    shift
    goto :parse_args
)
if /I "%~1"=="--no-strip" (
    set "STRIP_OUTPUT=0"
    shift
    goto :parse_args
)
if /I "%~1"=="--help" goto :help
echo Unknown option: %~1
goto :usage

:help
echo Usage: %~nx0 [--abi arm64-v8a] [--config Release] [--platform android-24] [--jobs N] [--ndk PATH] [--clean] [--no-strip]
echo Output:  bin\android\ABI\CONFIG\libZRuntimeShared.so
echo Symbols: bin\android\symbols\CONFIG\ABI\libZRuntimeShared.so
exit /b 0

:usage
echo Usage: %~nx0 [--abi arm64-v8a] [--config Release] [--platform android-24] [--jobs N] [--ndk PATH] [--clean] [--no-strip]
echo Output:  bin\android\ABI\CONFIG\libZRuntimeShared.so
echo Symbols: bin\android\symbols\CONFIG\ABI\libZRuntimeShared.so
exit /b 1

:main
if not defined ANDROID_NDK_ROOT if defined ANDROID_NDK set "ANDROID_NDK_ROOT=%ANDROID_NDK%"
if not defined ANDROID_NDK_ROOT if exist "C:\android-ndk-r25c-windows-x86_64\build\cmake\android.toolchain.cmake" set "ANDROID_NDK_ROOT=C:\android-ndk-r25c-windows-x86_64"
if not defined ANDROID_NDK_ROOT (
    echo [ERROR] ANDROID_NDK_ROOT is not defined. Use --ndk PATH or set ANDROID_NDK_ROOT.
    exit /b 1
)
if not exist "%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake" (
    echo [ERROR] Android toolchain not found: %ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake
    exit /b 1
)

set "MAKE_EXE=C:\Strawberry\c\bin\mingw32-make.exe"
if not exist "%MAKE_EXE%" set "MAKE_EXE=mingw32-make"
set "NDK_HOST_TAG=windows-x86_64"
if exist "%ANDROID_NDK_ROOT%\toolchains\llvm\prebuilt\windows-arm64\bin\llvm-strip.exe" set "NDK_HOST_TAG=windows-arm64"
set "STRIP_EXE=%ANDROID_NDK_ROOT%\toolchains\llvm\prebuilt\%NDK_HOST_TAG%\bin\llvm-strip.exe"
set "BUILD_DIR=%ROOT_DIR%\build\lib\android\%ABI%\%CONFIG%"
set "OUTPUT_SUBDIR=android/%ABI%"
set "OUT_SO=%ROOT_DIR%\bin\android\%ABI%\%CONFIG%\libZRuntimeShared.so"
set "SYMBOL_SO=%ROOT_DIR%\bin\android\symbols\%CONFIG%\%ABI%\libZRuntimeShared.so"

echo ========================================
echo ZRuntimeShared Android Build
echo ========================================
echo Root:   %ROOT_DIR%
echo NDK:    %ANDROID_NDK_ROOT%
echo ABI:    %ABI%
echo Config: %CONFIG%
echo Output: %OUT_SO%
echo Symbols:%SYMBOL_SO%
echo Strip:  %STRIP_OUTPUT%
echo.

if "%CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE="%CONFIG%" ^
  -DCMAKE_MAKE_PROGRAM="%MAKE_EXE%" ^
  -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_ROOT%/build/cmake/android.toolchain.cmake" ^
  -DANDROID_NDK="%ANDROID_NDK_ROOT%" ^
  -DANDROID_ABI="%ABI%" ^
  -DANDROID_PLATFORM="%ANDROID_PLATFORM%" ^
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON ^
  -DTARGET_PLATFORM:STRING=android ^
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF ^
  -DZENGINE_MOBILE_SIZE_OPTIMIZE=ON ^
  -DZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS=ON ^
  -DZENGINE_OUTPUT_SUBDIR="%OUTPUT_SUBDIR%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --target ZRuntimeShared -- -j%PARALLEL_JOBS%
if errorlevel 1 exit /b 1

if "%STRIP_OUTPUT%"=="1" (
    if not exist "%STRIP_EXE%" (
        echo [ERROR] llvm-strip not found: %STRIP_EXE%
        exit /b 1
    )
    if not exist "%OUT_SO%" (
        echo [ERROR] Output library not found: %OUT_SO%
        exit /b 1
    )
    for %%I in ("%SYMBOL_SO%") do set "SYMBOL_DIR=%%~dpI"
    if not exist "!SYMBOL_DIR!" mkdir "!SYMBOL_DIR!"
    copy /Y "%OUT_SO%" "%SYMBOL_SO%" >nul
    "%STRIP_EXE%" -s "%OUT_SO%"
    if errorlevel 1 exit /b 1
)

echo.
echo [SUCCESS] %OUT_SO%
if "%STRIP_OUTPUT%"=="1" echo [SYMBOLS] %SYMBOL_SO%
exit /b 0
