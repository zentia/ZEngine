#!/bin/bash
# ZEngine Generate Script for macOS
# This script generates the project files using CMake presets

set -e

# Get script directory
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" 1>/dev/null 2>/dev/null && pwd)"
cd "${ROOT_DIR}"

print_usage() {
    echo "Usage: ./gen_macos.sh [options]"
    echo ""
    echo "Options:"
    echo "  --papi TYPE        Set PAPI type: quickjs, v8 (default: quickjs)"
    echo "  --unity           Enable Unity Build"
    echo "  --fast            Alias of --unity"
    echo "  --xcode           Use Xcode generator (default)"
    echo "  --ninja           Use Ninja generator"
    echo "  --debug           Configure Debug (default)"
    echo "  --release         Configure Release"
    echo "  --config CONFIG   Configure debug or release"
    echo "  --preset NAME     Use a specific CMake preset"
    echo "  -h, --help        Show this help"
    echo ""
    echo "Examples:"
    echo "  ./gen_macos.sh"
    echo "  ./gen_macos.sh --release"
    echo "  ./gen_macos.sh --ninja --papi lua"
    echo "  ./gen_macos.sh --fast --config release"
}

# Parse command line arguments
# PAPI_TYPE: ZEngine is JS-only. Supported VMs: quickjs (default, mobile/web) | v8 (desktop/console).
PAPI_TYPE="quickjs"
USE_UNITY_BUILD="OFF"
CONFIG="debug"
GENERATOR="xcode"
PRESET_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --papi)
            if [[ -z "${2:-}" ]]; then
                echo "[ERROR] --papi requires a value"
                print_usage
                exit 1
            fi
            PAPI_TYPE="$2"
            shift 2
            ;;
        --unity)
            USE_UNITY_BUILD="ON"
            shift
            ;;
        --fast)
            USE_UNITY_BUILD="ON"
            shift
            ;;
        --xcode)
            GENERATOR="xcode"
            shift
            ;;
        --ninja)
            GENERATOR="ninja"
            shift
            ;;
        --debug)
            CONFIG="debug"
            shift
            ;;
        --release)
            CONFIG="release"
            shift
            ;;
        --config)
            if [[ -z "${2:-}" ]]; then
                echo "[ERROR] --config requires a value"
                print_usage
                exit 1
            fi
            CONFIG="$(echo "$2" | tr '[:upper:]' '[:lower:]')"
            shift 2
            ;;
        --preset)
            if [[ -z "${2:-}" ]]; then
                echo "[ERROR] --preset requires a value"
                print_usage
                exit 1
            fi
            PRESET_NAME="$2"
            shift 2
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Validate PAPI_TYPE -- JS-only.
case "${PAPI_TYPE}" in
    quickjs|v8)
        ;;
    *)
        echo "[ERROR] Invalid PAPI type: ${PAPI_TYPE}"
        echo "[INFO] Valid options: quickjs, v8"
        exit 1
        ;;
esac

# Validate CONFIG
case "${CONFIG}" in
    debug|release)
        ;;
    *)
        echo "[ERROR] Invalid config: ${CONFIG}"
        echo "[INFO] Valid options: debug, release"
        exit 1
        ;;
esac

# Determine preset name unless explicitly specified
if [[ -z "${PRESET_NAME}" ]]; then
    case "${GENERATOR}" in
        xcode)
            PRESET_NAME="macos_xcode_${CONFIG}"
            ;;
        ninja)
            PRESET_NAME="macos_ninja_${CONFIG}"
            ;;
        *)
            echo "[ERROR] Invalid generator: ${GENERATOR}"
            echo "[INFO] Valid options: xcode, ninja"
            exit 1
            ;;
    esac
fi

CONFIG_DISPLAY="Debug"
if [[ "${CONFIG}" == "release" ]]; then
    CONFIG_DISPLAY="Release"
fi

# Get CPU count for display
CPU_COUNT="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if [[ -z "${CPU_COUNT}" ]]; then
    CPU_COUNT="4"
fi

echo ""
echo "========================================"
echo "ZEngine Project Generation"
echo "========================================"
echo ""
echo "[INFO] PAPI Type: ${PAPI_TYPE}"
echo "[INFO] Config: ${CONFIG_DISPLAY}"
echo "[INFO] Generator: ${GENERATOR}"
echo "[INFO] Preset: ${PRESET_NAME}"
if [[ "${USE_UNITY_BUILD}" == "ON" ]]; then
    echo "[INFO] Unity Build: Enabled"
else
    echo "[INFO] Unity Build: Disabled"
fi
echo ""
echo "[TIP] For faster builds and better CPU utilization:"
echo ""
echo "  === Xcode IDE Builds ==="
echo "  1. Xcode can build multiple targets/files in parallel by default."
echo "  2. Command line build: cmake --build build --config ${CONFIG_DISPLAY} --parallel ${CPU_COUNT}"
echo ""
echo "  === Best Performance: Use Ninja Generator ==="
echo "  3. Use --ninja flag: ./gen_macos.sh --ninja"
echo "     Then build: cmake --build build --parallel ${CPU_COUNT}"
echo "     Ninja is typically faster than the Xcode generator for command line builds."
echo ""
echo "  === Other Options ==="
echo "  4. Use --fast to enable Unity Builds (faster full builds, slower incremental)."
echo "  5. Example: ./gen_macos.sh --ninja --papi lua"
echo ""

if ! command -v cmake >/dev/null 2>&1; then
    echo "[ERROR] CMake is not installed or not in PATH"
    exit 1
fi

if [[ "${GENERATOR}" == "xcode" ]] && ! command -v xcodebuild >/dev/null 2>&1; then
    echo "[WARNING] xcodebuild not found. Please install Xcode or Command Line Tools."
fi

if [[ "${GENERATOR}" == "ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    echo "[WARNING] ninja not found. Install Ninja or use the default Xcode generator."
fi

EXTRA_ARGS=("-DPAPI_TYPE=${PAPI_TYPE}" "-DUSE_UNITY_BUILD=${USE_UNITY_BUILD}")

# Check if Python is available (for unified tool)
PYTHON_CMD=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON_CMD="python3"
elif command -v python >/dev/null 2>&1; then
    PYTHON_CMD="python"
fi

if [[ -z "${PYTHON_CMD}" ]]; then
    echo "[WARNING] Python not found, using CMake directly"
    cmake --preset "${PRESET_NAME}" "${EXTRA_ARGS[@]}"
else
    echo "[INFO] Using unified build tool..."
    "${PYTHON_CMD}" zbuild.py configure --preset "${PRESET_NAME}" --extra-args "${EXTRA_ARGS[@]}"
fi

if [[ $? -ne 0 ]]; then
    echo "[ERROR] Generation failed"
    exit 1
fi

echo ""
echo "[SUCCESS] Project files generated successfully!"
echo ""
