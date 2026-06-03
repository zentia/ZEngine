@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "ROOT_DIR=%%~fI"

set "CONFIG=Release"
set "ABI=arm64-v8a"
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
if /I "%~1"=="--jobs" (
    set "PARALLEL_JOBS=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--ohos-sdk" (
    set "OHOS_SDK=%~2"
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
echo Usage: %~nx0 [--abi arm64-v8a] [--config Release] [--jobs N] [--ohos-sdk PATH] [--clean] [--no-strip]
echo Output:  bin\ohos\ABI\CONFIG\libZRuntimeShared.so
echo Symbols: bin\ohos\symbols\CONFIG\ABI\libZRuntimeShared.so
exit /b 0

:usage
echo Usage: %~nx0 [--abi arm64-v8a] [--config Release] [--jobs N] [--ohos-sdk PATH] [--clean] [--no-strip]
echo Output:  bin\ohos\ABI\CONFIG\libZRuntimeShared.so
echo Symbols: bin\ohos\symbols\CONFIG\ABI\libZRuntimeShared.so
exit /b 1

:main
if not defined OHOS_SDK if exist "%LOCALAPPDATA%\OpenHarmony\Sdk\12\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK=%LOCALAPPDATA%\OpenHarmony\Sdk\12"
if not defined OHOS_SDK if exist "C:\Users\Administrator\AppData\Local\OpenHarmony\Sdk\12\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK=C:\Users\Administrator\AppData\Local\OpenHarmony\Sdk\12"
if not defined OHOS_SDK (
    echo [ERROR] OHOS_SDK is not defined. Use --ohos-sdk PATH or set OHOS_SDK.
    exit /b 1
)

set "NDK_PATH=%OHOS_SDK%\native"
if not exist "%NDK_PATH%\build\cmake\ohos.toolchain.cmake" (
    echo [ERROR] OHOS toolchain not found: %NDK_PATH%\build\cmake\ohos.toolchain.cmake
    exit /b 1
)

set "MAKE_EXE=C:\Strawberry\c\bin\gmake.exe"
if not exist "%MAKE_EXE%" set "MAKE_EXE=gmake"
set "STRIP_EXE=%NDK_PATH%\llvm\bin\llvm-strip.exe"
if not exist "%STRIP_EXE%" set "STRIP_EXE=%NDK_PATH%\llvm\bin\llvm-strip"
set "BUILD_DIR=%ROOT_DIR%\build\lib\ohos\%ABI%\%CONFIG%"
set "OUTPUT_SUBDIR=ohos/%ABI%"
set "NDK_POSIX=%NDK_PATH:\=/%"
set "OUT_SO=%ROOT_DIR%\bin\ohos\%ABI%\%CONFIG%\libZRuntimeShared.so"
set "SYMBOL_SO=%ROOT_DIR%\bin\ohos\symbols\%CONFIG%\%ABI%\libZRuntimeShared.so"

echo ========================================
echo ZRuntimeShared OHOS Build
echo ========================================
echo Root:   %ROOT_DIR%
echo SDK:    %OHOS_SDK%
echo ABI:    %ABI%
echo Config: %CONFIG%
echo Output: %OUT_SO%
echo Symbols:%SYMBOL_SO%
echo Strip:  %STRIP_OUTPUT%
echo.

if "%CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -G "Unix Makefiles" ^
  -DCMAKE_BUILD_TYPE="%CONFIG%" ^
  -DCMAKE_MAKE_PROGRAM="%MAKE_EXE%" ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK_POSIX%/build/cmake/ohos.toolchain.cmake" ^
  -DOHOS_ARCH="%ABI%" ^
  -DOHOS_PLATFORM=ohos ^
  -DOHOS_STL=c++_static ^
  -DTARGET_PLATFORM:STRING=ohos ^
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF ^
  -DGLFW_BUILD_X11=OFF ^
  -DGLFW_BUILD_WAYLAND=OFF ^
  -DCURL_ENABLE_SSL=OFF ^
  -DCURL_USE_OPENSSL=OFF ^
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
