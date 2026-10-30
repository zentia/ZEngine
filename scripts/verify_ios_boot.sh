#!/usr/bin/env bash
# =============================================================================
# verify_ios_boot.sh —— 在 iOS 模拟器上**真跑一次**引擎 boot
#
# 为什么需要它：2026-10-30 在 iOS 上遇到四个"编译得过、起不来"的缺陷 ——
#   1. iOS 26+ SDK 要求 UIScene 生命周期，否则启动即退；
#   2. .app 里没有 ZEditor.ini，ConfigManager 把它当致命错误，39 个系统全回滚；
#   3. ProjectInfo 拿不到工程路径（两个静默原因叠加）；
#   4. 粒子纹理没读到，LoadTextureHDR 返回 nullptr 却照样解引用 → 首帧崩。
# 四个全是编译期零报错、链接期零报错、装上才炸，
# .github/workflows/build_ios.yml 的**编译闸门一个都发现不了**。
# 这个脚本就是为那一类缺陷准备的：它真的把 app 装进模拟器跑起来。
#
# 为什么用模拟器而不是真机：模拟器**不需要签名**，是唯一能在没有 Team ID /
# 没有 Apple ID / 没有设备的机器上执行 iOS 代码的地方。
#
# 断言（每条都做过正向 + 负向实跑，见 doc/MOBILE_PACKAGING.md §3.11）：
#   1. 引擎日志里出现 "ZPlayer: booted"        —— 拦启动即退（上面的 1）
#   2. 引擎日志里出现 "MetalRHI: swapchain ready" —— 拦 RHI 没起来
#   3. 引擎日志里没有 [F]（fatal）行           —— 拦系统初始化失败（上面的 2、3）
#   4. 运行期间没有产生新的崩溃报告 .ips        —— 拦空指针崩溃（上面的 4）
#
# 用法：
#   scripts/verify_ios_boot.sh [app_bundle 路径]
# 环境变量：
#   IOS_SIM_UDID   指定模拟器（默认：先用已 Booted 的，否则起一个 iPhone）
#   IOS_BOOT_WAIT  跑多少秒再检查（默认 15）
# =============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

APP="${1:-build/iossim/engine/Source/Player/IOS/ZEngine.app}"
BUNDLE_ID="com.zengine.player"
WAIT="${IOS_BOOT_WAIT:-15}"
REPORTS="${HOME}/Library/Logs/DiagnosticReports"

fail() {
    echo "::error::$*"
    echo "verify_ios_boot: FAIL"
    exit 1
}

echo "==> app: $APP"
[ -d "$APP" ] || fail "app bundle not found -- build it first: ninja -C build/iossim ZGameIOS"
[ -f "$APP/ZEngine" ] || fail "$APP/ZEngine is missing (did OUTPUT_NAME change?)"

# -----------------------------------------------------------------------------
# 1. 选一个模拟器：已 Booted 的优先，否则 boot 一个 iPhone。
#    用 `simctl list -j` + python 解析而不是 grep 文本 —— 本机沙箱里 bash 的
#    grep 会静默返回空，脚本会退化成"永远找不到设备"。
# -----------------------------------------------------------------------------
UDID="${IOS_SIM_UDID:-}"
if [ -z "$UDID" ]; then
    UDID="$(python3 - <<'PY'
import json, subprocess

raw = subprocess.run(["xcrun", "simctl", "list", "devices", "-j"],
                     capture_output=True, text=True).stdout
try:
    devices = json.loads(raw).get("devices", {})
except ValueError:
    devices = {}

booted, idle = [], []
for runtime, items in devices.items():
    if "iOS" not in runtime:
        continue
    for d in items:
        if not d.get("isAvailable", False):
            continue
        name, state = d.get("name", ""), d.get("state", "")
        if state == "Booted":
            booted.append(d.get("udid", ""))
        elif "iPhone" in name and state == "Shutdown":
            idle.append(d.get("udid", ""))

print(next((u for u in booted + idle if u), ""))
PY
)"
fi
[ -n "$UDID" ] || fail "no usable iPhone simulator (xcrun simctl list devices -j)"

echo "==> simulator: $UDID"
xcrun simctl boot "$UDID" >/dev/null 2>&1 || true   # 已经 Booted 时是无害的空操作
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || true

# -----------------------------------------------------------------------------
# 2. 先 uninstall 再 install：容器 UUID 每次 install 都可能变，而不清掉的话
#    日志是**累积**的，[F] 断言会读到上一次运行留下的行 —— 那等于没断言。
# -----------------------------------------------------------------------------
xcrun simctl uninstall "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP" || fail "simctl install failed"

BEFORE="$(ls -t "$REPORTS" 2>/dev/null | head -1 || true)"

LAUNCH_OUT="$(xcrun simctl launch "$UDID" "$BUNDLE_ID" 2>&1)" \
    || fail "simctl launch failed: $LAUNCH_OUT"
echo "==> launched: $LAUNCH_OUT"

echo "==> running for ${WAIT}s ..."
sleep "$WAIT"

# -----------------------------------------------------------------------------
# 3. 断言 4：没有新的崩溃报告。
#    （沙箱里 ps 会报 operation not permitted，所以不能用"进程还在不在"判断。）
# -----------------------------------------------------------------------------
AFTER="$(ls -t "$REPORTS" 2>/dev/null | head -1 || true)"
if [ -n "$AFTER" ] && [ "$AFTER" != "$BEFORE" ]; then
    fail "app crashed during the run: $AFTER"
fi
echo "  [ok] no crash report was produced"

# -----------------------------------------------------------------------------
# 4. 定位引擎日志
# -----------------------------------------------------------------------------
LOG="$(ls -td "${HOME}"/Library/Developer/CoreSimulator/Devices/"$UDID"/data/Containers/Data/Application/*/Library/Logs/ZEngine/*.log 2>/dev/null | head -1 || true)"
[ -n "$LOG" ] || fail "no engine log in the simulator container -- the app never got far enough to open it"
echo "==> log: $LOG"

# 子串匹配（index）而不是正则：日志里有 []、()、/ 等字符，转义容易写错。
assert_log_has() {
    if ! awk -v pat="$1" 'index($0, pat) { found = 1 } END { exit !found }' "$LOG"; then
        fail "missing expected log line: '$1' ($2)"
    fi
    echo "  [ok] $2"
}

# -----------------------------------------------------------------------------
# 5. 断言 1 / 2 / 3
# -----------------------------------------------------------------------------
assert_log_has "ZPlayer: booted" "engine finished booting"
assert_log_has "MetalRHI: swapchain ready" "Metal swapchain created"

if awk '/\[F\]/ { found = 1 } END { exit !found }' "$LOG"; then
    echo "--- [F] lines in the log ---"
    awk '/\[F\]/ { print "    " $0 }' "$LOG" | head -20
    fail "log has [F] (fatal) lines -- a system failed to initialize"
fi
echo "  [ok] no [F] fatal lines"

# 跑完收个尾，别把 app 留在前台占着模拟器。
xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true

echo "verify_ios_boot: PASS"
