#!/bin/bash
# ============================================================================
# WSL bootstrap — 在 WSL(Ubuntu) 里一键闭环 ZEngine 的三项「待真机」验证：
#   #2  桌面 POSIX fork 崩溃捕获路径（PosixCrashCaptureSmokeTest）
#   #1/#4 Linux RHI 运行期（RHIFactoryRegistrySmokeTest，GetDefaultAPI()==Vulkan）
#   #C7 Linux X11 窗口层 + XRandR 多显示器（LinuxX11WindowLayerSmokeTest，commit 6a0a578b）
#        - 无头降级：unset DISPLAY 时 Initialize() 返回 false 不崩
#        - 真窗口：  Xvfb 虚拟帧缓冲下建窗 + PollEvents + 多显示器枚举 + 子窗口
#
# 用法（在 WSL 里，仓库挂到 /mnt/e/Engine/ZEngine 或你 clone 的位置）：
#   cd <ZEngine>
#   ./scripts/wsl_verify_linux.sh            # 装依赖 + 构建 + 跑全部 smoke test
#   ./scripts/wsl_verify_linux.sh --deps-only  # 只装依赖
#   ./scripts/wsl_verify_linux.sh --build-only # 只构建（假定依赖已装）
#   ./scripts/wsl_verify_linux.sh --no-xvfb    # 跳过 Xvfb 真窗口路径（只跑无头）
#
# 幂等：依赖用 apt 装，构建走 build_linux.sh（增量）。可反复跑。
# 退出码：0 = 全部通过；非 0 = 有检查失败（末尾汇总）。
# ============================================================================
set -uo pipefail

MY_DIR="$(cd "$(dirname "$0")/.." 1>/dev/null 2>/dev/null && pwd)"
cd "${MY_DIR}"

DEPS_ONLY=0
BUILD_ONLY=0
NO_XVFB=0
for arg in "$@"; do
    case "$arg" in
        --deps-only)  DEPS_ONLY=1 ;;
        --build-only) BUILD_ONLY=1 ;;
        --no-xvfb)    NO_XVFB=1 ;;
        *) echo "unknown arg: $arg"; exit 1 ;;
    esac
done

# 失败计数：不因单项失败中断整轮，末尾统一汇总（verdict: PASS/FAIL）。
FAILURES=0
TOTAL=0
run_check() {
    # run_check <label> <binary-path> [env... 前缀的变量赋值]
    local label="$1"; shift
    local bin="$1"; shift
    TOTAL=$((TOTAL + 1))
    echo "==> [run] ${label} ..."
    if [ -z "${bin}" ] || [ ! -x "${bin}" ]; then
        echo "    [WARN] binary not found or not executable: '${bin}'"
        FAILURES=$((FAILURES + 1))
        return
    fi
    echo "    binary: ${bin}"
    "$@" "${bin}"
    local rc=$?
    if [ "${rc}" -eq 0 ]; then
        echo "    => PASS (exit=0)"
    else
        echo "    => FAIL (exit=${rc})"
        FAILURES=$((FAILURES + 1))
    fi
}

# --- 1. 系统依赖（Ubuntu/Debian）-------------------------------------------
# 桌面 POSIX 崩溃捕获：gcc/g++、cmake、ninja、python3。
# Linux RHI：libvulkan-dev（find_package(Vulkan REQUIRED)）+ glslang-tools。
# Linux X11 窗口层：libx11-dev libxrandr-dev（find_package(X11/Xrandr)）+
#   libxrender-dev/libxcursor-dev/libxi-dev/libxfixes-dev（X11 扩展，链接更稳）。
# 真窗口路径：xvfb（虚拟帧缓冲 X server，无显示器也能建窗验证）。
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
        libx11-dev \
        libxrandr-dev \
        libxrender-dev \
        libxcursor-dev \
        libxi-dev \
        libxfixes-dev \
        xvfb \
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

# --- 2. 构建三个 smoke test 目标 --------------------------------------------
echo ""
echo "==> [build] configuring + building smoke tests (debug, ninja)..."
# build_linux.sh 走 zbuild.py；先 configure 再按 target 构建。
./build_linux.sh debug ninja
echo "==> [build] building PosixCrashCaptureSmokeTest..."
./build_linux.sh debug ninja --target PosixCrashCaptureSmokeTest
echo "==> [build] building RHIFactoryRegistrySmokeTest..."
./build_linux.sh debug ninja --target RHIFactoryRegistrySmokeTest
echo "==> [build] building LinuxX11WindowLayerSmokeTest..."
./build_linux.sh debug ninja --target LinuxX11WindowLayerSmokeTest

# --- 3. 定位可执行文件 -------------------------------------------------------
# 可执行文件可能落在 build/ 或 bin/Debug（RUNTIME_OUTPUT 未显式设，CMake 默认）。
# 全树搜索最稳妥。
find_bin() {
    find . -name "$1" -type f -not -name '*.cpp' 2>/dev/null | head -1
}
POWSH_TEST="$(find_bin PosixCrashCaptureSmokeTest)"
RHI_TEST="$(find_bin RHIFactoryRegistrySmokeTest)"
X11_TEST="$(find_bin LinuxX11WindowLayerSmokeTest)"

# --- 4. 运行验证 -------------------------------------------------------------
echo ""
echo "==> [run] PosixCrashCaptureSmokeTest (#2 desktop POSIX fork path) ..."
run_check "PosixCrashCaptureSmokeTest (#2 desktop POSIX fork path)" "${POWSH_TEST}"

echo ""
echo "==> [run] RHIFactoryRegistrySmokeTest (#1/#4 Linux RHI runtime) ..."
run_check "RHIFactoryRegistrySmokeTest (#1/#4 Linux RHI runtime)" "${RHI_TEST}"

# ---- 4a. X11 窗口层：无头降级路径（强制无 DISPLAY）-------------------------
# LinuxX11WindowLayerSmokeTest 内部会用一个不存在的 display(:99.99) 强制走
# XOpenDisplay 失败分支，验证 Initialize() 返回 false 不崩、GetNativeDisplay()
# 保持 null。故此处即使当前有 DISPLAY 也能闭环验证降级路径。
echo ""
echo "==> [run] LinuxX11WindowLayerSmokeTest (#C7 X11 window layer, headless) ..."
run_check "LinuxX11WindowLayerSmokeTest (headless degrade path)" "${X11_TEST}"

# ---- 4b. X11 窗口层：真窗口路径（Xvfb 虚拟帧缓冲）--------------------------
# 有 DISPLAY 时 smoke test 才走真窗口：建窗 + PollEvents + XRandR 多显示器枚举
# + 子窗口生命周期 + 剪贴板。用 Xvfb 起一个虚拟 X server，跑完即销毁。
if [ "${NO_XVFB}" -eq 1 ]; then
    echo ""
    echo "==> [skip] --no-xvfb: skipping Xvfb real-window path."
elif ! command -v Xvfb >/dev/null 2>&1; then
    echo ""
    echo "==> [skip] Xvfb not installed; skipping real-window path (run --deps-only to install)."
else
    echo ""
    echo "==> [run] LinuxX11WindowLayerSmokeTest (#C7 X11 window layer, Xvfb) ..."
    XVFB_DISPLAY=:99
    XVFB_LOCK="/tmp/.X${XVFB_DISPLAY#*:}-lock"
    XVFB_PID=""
    cleanup_xvfb() {
        if [ -n "${XVFB_PID}" ]; then
            kill "${XVFB_PID}" 2>/dev/null || true
            wait "${XVFB_PID}" 2>/dev/null || true
        fi
        rm -f "${XVFB_LOCK}" 2>/dev/null || true
    }
    trap cleanup_xvfb EXIT INT TERM

    # 起虚拟帧缓冲（1280x720x24）。-ac 关访问控制，本地回环足够。
    echo "    starting Xvfb ${XVFB_DISPLAY} (1280x720x24) ..."
    Xvfb "${XVFB_DISPLAY}" -screen 0 1280x720x24 -ac +extension RANDR >/tmp/xvfb_zengine.log 2>&1 &
    XVFB_PID=$!

    # 等 server 就绪（最多 ~10s）。
    ready=0
    for i in $(seq 1 50); do
        if xdpyinfo -display "${XVFB_DISPLAY}" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 0.2
    done
    if [ "${ready}" -ne 1 ]; then
        echo "    [WARN] Xvfb did not become ready; see /tmp/xvfb_zengine.log"
        FAILURES=$((FAILURES + 1))
        TOTAL=$((TOTAL + 1))
    else
        export DISPLAY="${XVFB_DISPLAY}"
        run_check "LinuxX11WindowLayerSmokeTest (Xvfb real-window path)" "${X11_TEST}"
        unset DISPLAY
    fi
    cleanup_xvfb
    trap - EXIT INT TERM
fi

# --- 5. 汇总 -----------------------------------------------------------------
echo ""
echo "==> WSL Linux verification complete."
echo "    total checks: ${TOTAL}, failed: ${FAILURES}"
if [ "${FAILURES}" -eq 0 ]; then
    echo "    VERDICT: PASS"
    exit 0
else
    echo "    VERDICT: FAIL (${FAILURES} failed)"
    exit 1
fi
