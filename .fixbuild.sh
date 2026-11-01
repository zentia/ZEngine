#!/usr/bin/env bash
# 全量构建 + 「逐个补链」。
#
# 为什么需要这个脚本
# ------------------
# LNK1236（`ZRuntime.lib(Transform.obj) : corrupt or invalid COFF sections`）在
# 这台机器上会**随单次 MSBuild 进程内已链接的项目数增加而变多**：一遍全量里
# 前面 20 个 exe 都好，后面开始零星失败，越往后越密，而且**每次失败的都不是
# 同一批**。单独 `--target` 重编那一个 exe 则 100% 成功。
#
# 结论是不要在一遍里把所有项目链完，而是：
#   1. 跑一遍全量（它会链掉绝大多数，失败的记下来）；
#   2. 把失败的 vcxproj 逐个单独重编（每次都是新的 MSBuild 进程）；
#   3. 再跑一遍全量确认。
# 重复 2/3 直到全绿。
set -u
cd /e/Engine/ZEngine || exit 1

unset https_proxy http_proxy HTTPS_PROXY HTTPProxy 2>/dev/null
export https_proxy= http_proxy= HTTPS_PROXY= HTTP_PROXY=

build_all() {
  env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY \
    cmake --build build --config Debug --parallel 1 > /tmp/fa.log 2>&1
  echo "  exit=$? linked=$(grep -c 'vcxproj ->' /tmp/fa.log) errors=$(grep -cE 'error C|error LNK|fatal error' /tmp/fa.log)"
}

failing_projects() {
  grep -oE '[A-Za-z0-9_\\:/.]+\.vcxproj\]' /tmp/fa.log \
    | sed 's/\]$//' | sort -u
}

for round in 1 2 3 4 5 6; do
  echo "=== round $round: full build ==="
  build_all
  projects=$(failing_projects)
  if [ -z "$projects" ]; then echo "ALL GREEN"; break; fi
  echo "  -> $(echo "$projects" | wc -l) project(s) to retry individually"
  echo "$projects" | while read -r p; do
    name=$(basename "$p" .vcxproj)
    env -u https_proxy -u http_proxy -u HTTPS_PROXY -u HTTP_PROXY \
      cmake --build build --target "$name" --config Debug --parallel 1 > /tmp/one.log 2>&1
    rc=$?
    echo "    [$rc] $name"
  done
done
echo "=== final verify ==="
build_all
grep -E "error C|error LNK|fatal error" /tmp/fa.log | head -20
