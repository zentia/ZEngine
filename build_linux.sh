#!/bin/bash
# ZEngine Build Script for Linux
# This script uses the unified Python build tool (zbuild.py)

set -e

echo ""
echo "========================================"
echo "ZEngine Build System"
echo "========================================"
echo ""

# Get script directory
MY_DIR="$(cd "$(dirname "$0")" 1>/dev/null 2>/dev/null && pwd)"
cd "${MY_DIR}"

# Check if Python is available
if ! command -v python3 &> /dev/null && ! command -v python &> /dev/null; then
    echo "[ERROR] Python is not installed or not in PATH"
    echo "Please install Python 3.6 or later"
    exit 1
fi

# Use python3 if available, otherwise python
PYTHON_CMD="python3"
if ! command -v python3 &> /dev/null; then
    PYTHON_CMD="python"
fi

# Check if zbuild.py exists
if [ ! -f "zbuild.py" ]; then
    echo "[ERROR] zbuild.py not found in current directory"
    exit 1
fi

# Parse arguments
CONFIG="debug"
GENERATOR=""
TARGET=""
JOBS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        debug|release)
            CONFIG="$1"
            shift
            ;;
        make|ninja)
            GENERATOR="$1"
            shift
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo ""
            echo "Usage: ./build_linux.sh [config] [generator] [--target TARGET] [--jobs N]"
            echo ""
            echo "config:"
            echo "  debug   -   build with the debug configuration"
            echo "  release -   build with the release configuration"
            echo ""
            echo "generator:"
            echo "  make    -   build with Unix Make"
            echo "  ninja   -   build with Ninja"
            echo ""
            exit 1
            ;;
    esac
done

# Create shader directory
mkdir -p "engine/shader/generated/spv"

# Configure
echo "[INFO] Configuring build..."
CONFIGURE_ARGS="configure --config ${CONFIG}"
if [ -n "${GENERATOR}" ]; then
    CONFIGURE_ARGS="${CONFIGURE_ARGS} --generator ${GENERATOR}"
fi

${PYTHON_CMD} zbuild.py ${CONFIGURE_ARGS}
if [ $? -ne 0 ]; then
    echo "[ERROR] Configuration failed"
    exit 1
fi

# Build
echo ""
echo "[INFO] Building project..."
BUILD_ARGS="build --config ${CONFIG}"
if [ -n "${TARGET}" ]; then
    BUILD_ARGS="${BUILD_ARGS} --target ${TARGET}"
fi
if [ -n "${JOBS}" ]; then
    BUILD_ARGS="${BUILD_ARGS} --jobs ${JOBS}"
fi

${PYTHON_CMD} zbuild.py ${BUILD_ARGS}
if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed"
    exit 1
fi

echo ""
echo "[SUCCESS] Build completed successfully!"
echo ""
