#!/usr/bin/env bash
# =============================================================================
# verify_nanite_dedup.sh — 编译并运行 Nanite 顶点去重可行性探针。
# -----------------------------------------------------------------------------
# 纯 CPU、headless，不依赖引擎/RHI/窗口。回答一个问题：把 NaniteCluster.cpp:277
# 的"每三角形独占 3 顶点"改成去重，索引会不会丢？
#
# 用法：
#   bash scripts/verify_nanite_dedup.sh
#
# 退出码：0 = 去重不丢索引（可行）；1 = 丢索引（不可行）。
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}/nanite_dedup_probe_out"
mkdir -p "$OUT"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    if command -v g++ >/dev/null 2>&1; then CXX="g++";
    elif command -v clang++ >/dev/null 2>&1; then CXX="clang++";
    else echo "ERROR: no g++/clang++ on PATH (set CXX=...)"; exit 2; fi
fi

# 老的 Strawberry Perl g++(<5) 不支持 -std=c++17；此时改走 MSVC（cl.exe）。
GCC_MAJOR=$("$CXX" -dumpversion 2>/dev/null | cut -d. -f1)
if [ "${GCC_MAJOR:-0}" -lt 5 ] 2>/dev/null; then
    echo "[warn] $CXX 太老（$("$CXX" --version | head -1)），改用 MSVC cl.exe"
    VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    VSPATH="$("$VSWHERE" -latest -property installationPath 2>/dev/null | tr -d '\r')"
    if [ -z "$VSPATH" ]; then echo "ERROR: vswhere 找不到 VS 安装路径"; exit 2; fi
    # 用 PowerShell 拉起 vcvars64 环境后编译（cl.exe 需 INCLUDE/LIB 环境变量）。
    WIN_HERE="$(cygpath -w "$HERE")"
    WIN_OUT="$(cygpath -w "$OUT")"
    powershell -NoProfile -Command "
        \$vsdevcmd = Join-Path '${VSPATH//\\/\\\\}' 'Common7\\Tools\\VsDevCmd.bat'
        cmd /c \"\`\"\$vsdevcmd\`\" -arch=amd64 && cl /nologo /std:c++17 /O2 /W4 /EHsc \`\"$WIN_HERE\\nanite_dedup_probe.cpp\`\" /Fe:\`\"$WIN_OUT\\nanite_dedup_probe.exe\`\"\"
    "
    echo "[run] $OUT/nanite_dedup_probe.exe"
    echo "------------------------------------------------------------"
    set +e
    "$OUT/nanite_dedup_probe.exe"
    RC=$?
    set -e
    echo "------------------------------------------------------------"
    echo "[exit] $RC"
    exit $RC
fi

echo "[build] $CXX -std=c++17 -O2 -Wall -Wextra nanite_dedup_probe.cpp"
"$CXX" -std=c++17 -O2 -Wall -Wextra \
    "$HERE/nanite_dedup_probe.cpp" -o "$OUT/nanite_dedup_probe"

echo "[run] nanite_dedup_probe"
echo "------------------------------------------------------------"
set +e
"$OUT/nanite_dedup_probe"
RC=$?
set -e
echo "------------------------------------------------------------"
echo "[exit] $RC"
exit $RC
