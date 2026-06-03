@echo off
setlocal enabledelayedexpansion

echo ========================================
echo ZEngine Platform Test Environment
echo ========================================
echo.

:: Check if CMake is available
cmake --version >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake is not installed or not in PATH
    echo Please install CMake and try again
    pause
    exit /b 1
)

:: Clean previous build
echo Cleaning previous build...
if exist build rmdir /s /q build
if exist test_results rmdir /s /q test_results

:: Create directories
mkdir build
mkdir test_results

echo.
echo ========================================
echo Building Platform Test
echo ========================================

:: Configure with CMake
echo Configuring with CMake...
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed
    pause
    exit /b 1
)

:: Build the project
echo Building project...
cmake --build build --config Debug
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed
    pause
    exit /b 1
)

echo.
echo ========================================
echo Running Platform Tests
echo ========================================

:: Run the platform test
echo Running platform detection test...
build\tests\Debug\PlatformTest.exe > test_results\platform_output.txt 2>&1

:: Display results
echo.
echo Test Results:
echo ========================================
type test_results\platform_output.txt

:: Run CTest if available
echo.
echo ========================================
echo Running CTest
echo ========================================
cd build
ctest --output-on-failure --verbose
cd ..

echo.
echo ========================================
echo Test Summary
echo ========================================

:: Check if test passed
findstr /C:"Platform:" test_results\platform_output.txt >nul
if %ERRORLEVEL% equ 0 (
    echo ✓ Platform detection test PASSED
) else (
    echo ✗ Platform detection test FAILED
)

findstr /C:"Architecture:" test_results\platform_output.txt >nul
if %ERRORLEVEL% equ 0 (
    echo ✓ Architecture detection test PASSED
) else (
    echo ✗ Architecture detection test FAILED
)

findstr /C:"Compiler:" test_results\platform_output.txt >nul
if %ERRORLEVEL% equ 0 (
    echo ✓ Compiler detection test PASSED
) else (
    echo ✗ Compiler detection test FAILED
)

echo.
echo Test results saved to: test_results\platform_output.txt
echo Build files located in: build\
echo.

pause
