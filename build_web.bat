@echo off
REM =============================================================================
REM ZEngine Web (Emscripten) Build Script
REM -----------------------------------------------------------------------------
REM Configures and builds the WebGL 2.0 (mini-program / browser) target via the
REM Emscripten toolchain. Mirrors build_windows.bat in style.
REM
REM Prerequisites:
REM   1. Install emsdk:        https://emscripten.org/docs/getting_started/downloads.html
REM   2. From your emsdk dir:  emsdk install latest && emsdk activate latest
REM   3. Source the env vars:  emsdk_env.bat
REM      (this script will refuse to continue if EMSDK is not set)
REM   4. Have Ninja on PATH (emsdk ships one; emcmake will pick it up).
REM
REM Usage:
REM   build_web.bat                              # debug build of ZWebLauncher (default)
REM   build_web.bat release                      # release build of ZWebLauncher
REM   build_web.bat debug --target ZRuntime      # build only the runtime static lib
REM   build_web.bat release --jobs 8             # custom job count
REM =============================================================================

setlocal enabledelayedexpansion

echo.
echo ========================================
echo ZEngine Web Build (Emscripten / WebGL2)
echo ========================================
echo.

REM --- Locate script directory and cd into it -------------------------------
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%" >nul

REM --- Sanity check: EMSDK must be configured -------------------------------
if "%EMSDK%"=="" (
    echo [ERROR] EMSDK environment variable is not set.
    echo         Please run "emsdk_env.bat" from your emsdk installation first.
    echo         Example:
    echo             cd C:\path\to\emsdk
    echo             emsdk install latest
    echo             emsdk activate latest
    echo             emsdk_env.bat
    popd >nul
    exit /b 1
)

REM --- Sanity check: emcmake must be on PATH --------------------------------
where emcmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] 'emcmake' was not found on PATH.
    echo         Make sure 'emsdk_env.bat' was sourced in this shell session.
    popd >nul
    exit /b 1
)

REM --- Argument parsing -----------------------------------------------------
set CONFIG=debug
set TARGET=ZWebLauncher
set JOBS=
REM PAPI_TYPE selects the scripting backend (quickjs | v8).
REM Web/Mini-Program builds MUST use QuickJS: V8 cannot run inside
REM emscripten/wasm in any practical form (V8 itself is a VM, double
REM interpretation kills perf and bloats size). QuickJS is a tiny pure-
REM interpreter JS VM (~400KB compiled to wasm) and is the natural pair
REM for a TypeScript-first scripting story (tsc -> .js -> QuickJS).
set PAPI_TYPE=quickjs

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="debug"          ( set "CONFIG=debug"          & shift & goto parse_args )
if /I "%~1"=="release"        ( set "CONFIG=release"        & shift & goto parse_args )
if /I "%~1"=="relwithdebinfo" ( set "CONFIG=relwithdebinfo" & shift & goto parse_args )
if /I "%~1"=="--target" (
    set "TARGET=%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--jobs" (
    set "JOBS=%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="--papi" (
    set "PAPI_TYPE=%~2"
    shift
    shift
    goto parse_args
)
echo [ERROR] Unknown option: %~1
echo.
echo Usage: build_web.bat [debug^|release^|relwithdebinfo] [--target NAME] [--jobs N] [--papi quickjs^|v8]
popd >nul
exit /b 1
:args_done

REM --- Map config -> preset suffix -----------------------------------------
set "PRESET="
if /I "%CONFIG%"=="debug"          set "PRESET=web_ninja_debug"
if /I "%CONFIG%"=="release"        set "PRESET=web_ninja_release"
if /I "%CONFIG%"=="relwithdebinfo" set "PRESET=web_ninja_relwithdebinfo"
if "%PRESET%"=="" (
    echo [ERROR] Unknown configuration: "%CONFIG%"
    echo         Expected one of: debug ^| release ^| relwithdebinfo
    popd >nul
    exit /b 1
)

REM --- Auto-detect job count if user did not pass --jobs --------------------
if "%JOBS%"=="" (
    for /f %%I in ('powershell -NoProfile -Command "[System.Environment]::ProcessorCount"') do set "JOBS=%%I"
)
if "%JOBS%"=="" set JOBS=4

echo [INFO] EMSDK            = %EMSDK%
echo [INFO] Configuration    = %CONFIG%
echo [INFO] CMake preset     = %PRESET%
echo [INFO] Build target     = %TARGET%
echo [INFO] PAPI type        = %PAPI_TYPE%
echo [INFO] Parallel jobs    = %JOBS%
echo.

REM --- Sanity check: Ninja must be on PATH ----------------------------------
REM emcmake will silently fall back to "MinGW Makefiles" if Ninja is missing,
REM which produces a build dir incompatible with the web_ninja_* preset and
REM leads to very confusing stale-cache failures on subsequent runs.
where ninja >nul 2>&1
if errorlevel 1 (
    echo [ERROR] 'ninja' was not found on PATH.
    echo         The web_ninja_* CMake preset requires the Ninja generator.
    echo         Install Ninja ^(https://github.com/ninja-build/ninja/releases^)
    echo         or add the ninja shipped with emsdk to your PATH, then retry.
    popd >nul
    exit /b 1
)

REM --- Guard: detect generator mismatch in existing build dir ---------------
REM If build/build_web_%CONFIG% was previously configured with a generator
REM other than Ninja (most commonly "MinGW Makefiles" when ninja was missing),
REM CMake will refuse to reconfigure or, worse, silently reuse stale rules.
REM Wipe the directory in that case so the preset can configure cleanly.
set "WEB_BUILD_DIR=build\build_web_%CONFIG%"
set "WEB_CACHE_FILE=%WEB_BUILD_DIR%\CMakeCache.txt"
if exist "%WEB_CACHE_FILE%" (
    set "CACHED_GENERATOR="
    for /f "usebackq tokens=2 delims==" %%G in (`findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "%WEB_CACHE_FILE%"`) do (
        set "CACHED_GENERATOR=%%G"
    )
    if /I not "!CACHED_GENERATOR!"=="Ninja" (
        echo [WARN] Existing build dir was configured with generator "!CACHED_GENERATOR!".
        echo        Expected "Ninja" ^(required by preset %PRESET%^).
        echo        Removing "%WEB_BUILD_DIR%" to avoid stale-cache issues ...
        rmdir /s /q "%WEB_BUILD_DIR%"
        if exist "%WEB_BUILD_DIR%" (
            echo [ERROR] Failed to remove "%WEB_BUILD_DIR%". Please delete it manually and retry.
            popd >nul
            exit /b 1
        )
    )
)

REM --- Configure step (must go through emcmake) -----------------------------
REM IMPORTANT: We pass -G Ninja explicitly. Without this, emcmake.py prefers
REM mingw32-make over ninja on Windows when both are present (e.g. Strawberry
REM Perl ships both), and silently appends `-G "MinGW Makefiles"` to the cmake
REM command line, which OVERRIDES the generator declared in the preset.
echo [INFO] Configuring web build (emcmake cmake --preset %PRESET% -G Ninja -DPAPI_TYPE=%PAPI_TYPE%) ...
call emcmake cmake --preset %PRESET% -G Ninja -DPAPI_TYPE=%PAPI_TYPE%
if errorlevel 1 (
    echo [ERROR] Configuration failed.
    popd >nul
    exit /b 1
)

REM --- Build step -----------------------------------------------------------
REM Note: we use --parallel instead of -j. With some CMake/Ninja combinations
REM `cmake --build --preset ... -j N` mis-forwards the job count to Ninja and
REM produces "ninja: fatal: invalid -j parameter". --parallel is the portable
REM CMake-level switch and gets translated correctly per generator.
echo.
echo [INFO] Building target '%TARGET%' ...
cmake --build --preset web_runtime_%CONFIG% --target %TARGET% --parallel %JOBS%
if errorlevel 1 (
    REM Fallback: some configurations (e.g. relwithdebinfo) do not have a build
    REM preset; drive the build directly from the binary directory instead.
    echo [WARN] Build preset not available, falling back to direct invocation.
    cmake --build "build/build_web_%CONFIG%" --target %TARGET% --parallel %JOBS%
    if errorlevel 1 (
        echo [ERROR] Build failed.
        popd >nul
        exit /b 1
    )
)

echo.
echo [SUCCESS] Web build completed: build\build_web_%CONFIG%\
popd >nul
endlocal
exit /b 0
