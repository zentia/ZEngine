#!/bin/bash

# ZEngine Platform Test Environment for Unix/Linux/macOS

set -e  # Exit on any error

echo "========================================"
echo "ZEngine Platform Test Environment"
echo "========================================"
echo

# Check if CMake is available
if ! command -v cmake &> /dev/null; then
    echo "ERROR: CMake is not installed or not in PATH"
    echo "Please install CMake and try again"
    exit 1
fi

# Clean previous build
echo "Cleaning previous build..."
rm -rf build test_results

# Create directories
mkdir -p build test_results

echo
echo "========================================"
echo "Building Platform Test"
echo "========================================"

# Configure with CMake
echo "Configuring with CMake..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
if [ $? -ne 0 ]; then
    echo "ERROR: CMake configuration failed"
    exit 1
fi

# Build the project
echo "Building project..."
cmake --build build --config Debug
if [ $? -ne 0 ]; then
    echo "ERROR: Build failed"
    exit 1
fi

echo
echo "========================================"
echo "Running Platform Tests"
echo "========================================"

# Run the platform test
echo "Running platform detection test..."
./build/tests/PlatformTest > test_results/platform_output.txt 2>&1

# Display results
echo
echo "Test Results:"
echo "========================================"
cat test_results/platform_output.txt

# Run CTest if available
echo
echo "========================================"
echo "Running CTest"
echo "========================================"
cd build
ctest --output-on-failure --verbose
cd ..

echo
echo "========================================"
echo "Test Summary"
echo "========================================"

# Check if test passed
if grep -q "Platform:" test_results/platform_output.txt; then
    echo "✓ Platform detection test PASSED"
else
    echo "✗ Platform detection test FAILED"
fi

if grep -q "Architecture:" test_results/platform_output.txt; then
    echo "✓ Architecture detection test PASSED"
else
    echo "✗ Architecture detection test FAILED"
fi

if grep -q "Compiler:" test_results/platform_output.txt; then
    echo "✓ Compiler detection test PASSED"
else
    echo "✗ Compiler detection test FAILED"
fi

echo
echo "Test results saved to: test_results/platform_output.txt"
echo "Build files located in: build/"
echo
