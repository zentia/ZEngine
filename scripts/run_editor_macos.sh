#!/bin/bash
# =====================================================================
# ZEditor (macOS / Metal) 一键构建 + 启动 —— 真机验收用
# ---------------------------------------------------------------------
# 做三件事：
#   1) 需要时 configure（ninja + Debug + 关 Vulkan，本机没有 Vulkan SDK）
#   2) 构建 ZEditor
#   3) 把 ZGameSmoke.zproject 放到可执行文件旁边并启动编辑器
#
# 用法：./scripts/run_editor_macos.sh
# 只构建不启动：./scripts/run_editor_macos.sh --no-launch
# =====================================================================

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." 1>/dev/null 2>/dev/null && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="${REPO_ROOT}/build"
BIN_DIR="${REPO_ROOT}/bin/Debug"
CMAKE_BIN="cmake"
CTEST_BIN="ctest"

# cmake/ninja 装在这台机器的用户目录下（系统 PATH 里没有）。
if [ -x "${HOME}/Library/Python/3.9/bin/cmake" ]; then
    export PATH="${HOME}/Library/Python/3.9/bin:${PATH}"
fi

LAUNCH=1
for arg in "$@"; do
    case "${arg}" in
        --no-launch) LAUNCH=0 ;;
        *) echo "Unknown option: ${arg}"; exit 1 ;;
    esac
done

# ---- 1) configure ----------------------------------------------------
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "[INFO] Configuring (ninja / Debug / Vulkan=OFF) ..."
    "${CMAKE_BIN}" -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DZENGINE_USE_VULKAN=OFF
else
    echo "[INFO] build/ already configured, skipping configure."
fi

# ---- 2) build --------------------------------------------------------
echo "[INFO] Building ZEditor ..."
"${CMAKE_BIN}" --build "${BUILD_DIR}" --target ZEditor

# ---- 3) 工程文件 ------------------------------------------------------
# bin/ 是 gitignore 的，install 只放 ZEditor.ini，工程文件得自己带过去。
mkdir -p "${BIN_DIR}"
cp -f "${REPO_ROOT}/engine/ZGameSmoke.zproject" "${BIN_DIR}/ZGameSmoke.zproject"

echo ""
echo "[SUCCESS] ZEditor ready: ${BIN_DIR}/ZEditor"
echo "          工程文件:      ${BIN_DIR}/ZGameSmoke.zproject"
echo "          引擎日志:      ~/Library/Logs/ZEngine/engine_<date>_1.log"

if [ "${LAUNCH}" -eq 0 ]; then
    exit 0
fi

# ---- 4) 启动 ----------------------------------------------------------
# 必须在 bin/Debug 里跑：ZEditor 按可执行文件所在目录找 resource/ 和 asset/。
echo "[INFO] Launching ZEditor ..."
cd "${BIN_DIR}"
./ZEditor -p ZGameSmoke.zproject
