#!/bin/bash
# ============================================================================
# WSL bootstrap — 在 WSL(Ubuntu) 里一键闭环 ZEngine 的两项「待真机」验证：
#   #2  桌面 POSIX fork 崩溃捕获路径（PosixCrashCaptureSmokeTest）
#   #1/#4 Linux RHI 运行期（RHIFactoryRegistrySmokeTest，GetDefaultAPI()==Vulkan）
#
# 用法（在 WSL 里，仓库挂到 /mnt/e/Engine/ZEngine 或你 clone 的位置）：
#   cd <ZEngine>
#   ./scripts/wsl_verify_linux.sh            # 装依赖 + 构建 + 跑两个 smoke test
#   ./scripts/wsl_verify_linux.sh --deps-only  # 只装依赖
#   ./scripts/wsl_verify_linux.sh --build-only # 只构建（假定依赖已装）
#
# 幂等：依赖用 apt 装，构建走 zbuild.py（增量）。可反复跑。
# ============================================================================
set -euo pipefail

MY_DIR="$(cd "$(dirname "$0")/.." 1>/dev/null 2>/dev/null && pwd)"
cd "${MY_DIR}"

DEPS_ONLY=0
BUILD_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --deps-only)  DEPS_ONLY=1 ;;
        --build-only) BUILD_ONLY=1 ;;
        *) echo "unknown arg: $arg"; exit 1 ;;
    esac
done

# --- 1. 系统依赖（Ubuntu/Debian）-------------------------------------------
# 桌面 POSIX 崩溃捕获需要 gcc/g++、cmake、ninja、python3；Linux RHI 需要
# libvulkan-dev（find_package(Vulkan REQUIRED)）+ glslang-tools（glslangValidator）。
install_deps() {
    echo "==> [deps] updating apt & installing build deps..."
    sudo apt-get update -y
    sudo apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        git \
        pkg-config \
        libvulkan-dev \
        glslang-tools \
        vulkan-tools \
        ca-certificates
    echo "==> [deps] done."
}

if [ "${BUILD_ONLY}" -eq 0 ]; then
    install_deps
fi

if [ "${DEPS_ONLY}" -eq 1 ]; then
    echo "==> --deps-only: dependencies installed, exiting."
    exit 0
fi

# --- 2. 构建两个 smoke test 目标 --------------------------------------------
echo ""
echo "==> [build] configuring + building smoke tests (debug, ninja)..."
# build_linux.sh 走 zbuild.py；先 configure 再按 target 构建。
./build_linux.sh debug ninja
echo "==> [build] building PosixCrashCaptureSmokeTest..."
./build_linux.sh debug ninja --target PosixCrashCaptureSmokeTest
echo "==> [build] building RHIFactoryRegistrySmokeTest..."
./build_linux.sh debug ninja --target RHIFactoryRegistrySmokeTest

# --- 3. 运行验证 -------------------------------------------------------------
echo ""
echo "==> [run] PosixCrashCaptureSmokeTest (#2 desktop POSIX fork path)..."
# 可执行文件可能落在 build/ 或 bin/Debug（RUNTIME_OUTPUT 未显式设，CMake 默认）。
# 全树搜索最稳妥。
POWSH_TEST="$(find . -name PosixCrashCaptureSmokeTest -type f -not -name '*.cpp' 2>/dev/null | head -1)"
if [ -z "${POWSH_TEST}" ]; then
    echo "    [WARN] PosixCrashCaptureSmokeTest binary not found; "
    echo "           locate it manually. Expected #2 fork-path verification."
else
    echo "    binary: ${POWSH_TEST}"
    cd "$(dirname "${POWSH_TEST}")"
    "./PosixCrashCaptureSmokeTest"
    echo "    exit=$?"
fi

echo ""
echo "==> [run] RHIFactoryRegistrySmokeTest (#1/#4 Linux RHI runtime)..."
RHI_TEST="$(find . -name RHIFactoryRegistrySmokeTest -type f 2>/dev/null | head -1)"
if [ -z "${RHI_TEST}" ]; then
    echo "    [WARN] RHIFactoryRegistrySmokeTest binary not found under build/."
else
    echo "    binary: ${RHI_TEST}"
    cd "$(dirname "${RHI_TEST}")"
    "./RHIFactoryRegistrySmokeTest"
    echo "    exit=$?"
fi

echo ""
echo "==> WSL Linux verification pass complete."
