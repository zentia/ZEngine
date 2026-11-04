#!/bin/bash
# Headless smoke test for the Linux build.
#
# Boots ZEditor against an Xvfb display, waits for the engine to reach
# "All systems initialized successfully", keeps it running for a few seconds and
# fails if the process dies (SIGSEGV, terminate(), or an early exit).
#
# This is the test that catches the class of bugs found while bringing Linux up:
# the X11 window layer refusing to create a window, and the async-read double
# free that only blew up a second into asset streaming.
#
# Usage:
#   scripts/smoke_linux_xvfb.sh [--seconds N] [--project PATH] [--binary PATH] [--keep-log]
#                                [--require-frame] [--frame-timeout SECONDS]
#
# Exit codes:
#   0  passed
#   1  the editor crashed or never reached the ready milestone
#   2  prerequisites missing (Xvfb / timeout / binary / project)
#
# Requirements: Xvfb, timeout(1), and a build produced by
#   python zbuild.py configure --preset linux_make_debug
#   cmake --build build --target ZEditor -j"$(nproc)"

set -u

MY_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${MY_DIR}/.." && pwd)"

RUN_SECONDS=20
PROJECT="${ROOT_DIR}/engine/ZGameSmoke.zproject"
BINARY="${ROOT_DIR}/bin/Debug/ZEditor"
KEEP_LOG=0
REQUIRE_FRAME=0
FRAME_TIMEOUT=120
READY_TIMEOUT=180

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds)  RUN_SECONDS="$2"; shift 2 ;;
        --project)  PROJECT="$2";     shift 2 ;;
        --binary)   BINARY="$2";      shift 2 ;;
        --keep-log) KEEP_LOG=1;       shift ;;
        --require-frame) REQUIRE_FRAME=1; shift ;;
        --frame-timeout) FRAME_TIMEOUT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "[ERROR] unknown argument: $1" >&2
            exit 2 ;;
    esac
done

log_info()  { echo "[smoke] $*"; }
log_warn()  { echo "[smoke][WARN] $*" >&2; }
log_error() { echo "[smoke][ERROR] $*" >&2; }

# ---------------------------------------------------------------- prerequisites
for tool in Xvfb timeout; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        log_error "'${tool}' not found. Install it (Debian/Ubuntu: xvfb / coreutils)."
        exit 2
    fi
done

if [ ! -x "${BINARY}" ]; then
    log_error "editor binary not found: ${BINARY}"
    log_error "build it first: cmake --build build --target ZEditor -j\"\$(nproc)\""
    exit 2
fi

if [ ! -f "${PROJECT}" ]; then
    log_error "project file not found: ${PROJECT}"
    exit 2
fi

# ------------------------------------------------------------------ Xvfb setup
pick_display() {
    local n=99
    while [ "${n}" -lt 120 ]; do
        if [ ! -e "/tmp/.X11-unix/X${n}" ]; then
            echo ":${n}"
            return 0
        fi
        n=$((n + 1))
    done
    return 1
}

SMOKE_DISPLAY="$(pick_display)" || { log_error "no free X display in :99..:119"; exit 2; }

XVFB_PID=""
EDITOR_PID=""

cleanup() {
    if [ -n "${EDITOR_PID}" ] && kill -0 "${EDITOR_PID}" 2>/dev/null; then
        kill -TERM "${EDITOR_PID}" 2>/dev/null
        wait "${EDITOR_PID}" 2>/dev/null
    fi
    if [ -n "${XVFB_PID}" ] && kill -0 "${XVFB_PID}" 2>/dev/null; then
        kill -TERM "${XVFB_PID}" 2>/dev/null
        wait "${XVFB_PID}" 2>/dev/null
    fi
    if [ "${KEEP_LOG}" -eq 0 ] && [ -n "${WORK_DIR:-}" ]; then
        rm -rf "${WORK_DIR}"
    fi
}
trap cleanup EXIT INT TERM

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zengine-smoke.XXXXXX")"
# 每次运行用独立的 state 目录：引擎日志按日期命名且会复用同名文件，混着上一次
# 的内容会让"启动就绪"在 1 秒内假阳性通过。
export XDG_STATE_HOME="${WORK_DIR}/state"
EDITOR_OUT="${WORK_DIR}/editor.out"
LOG_DIR="${XDG_STATE_HOME}/ZEngine/logs"
mkdir -p "${LOG_DIR}"

log_info "display   ${SMOKE_DISPLAY}"
log_info "binary    ${BINARY}"
log_info "project   ${PROJECT}"

Xvfb "${SMOKE_DISPLAY}" -screen 0 1280x720x24 > "${WORK_DIR}/xvfb.log" 2>&1 &
XVFB_PID=$!

for _ in $(seq 1 50); do
    [ -e "/tmp/.X11-unix/X${SMOKE_DISPLAY#:}" ] && break
    sleep 0.1
done

if ! kill -0 "${XVFB_PID}" 2>/dev/null; then
    log_error "Xvfb failed to start:"
    sed 's/^/    /' "${WORK_DIR}/xvfb.log" >&2
    exit 2
fi

# --------------------------------------------------------------- run the editor
# The engine writes its log into ${LOG_DIR}/engine_<date>_<n>.log and reuses the
# same file across runs today, so "a file that did not exist before" is not a
# reliable signal. Instead timestamp the launch and look at logs touched since.
START_EPOCH="$(date +%s)"

DISPLAY="${SMOKE_DISPLAY}" "${BINARY}" -p "${PROJECT}" > "${EDITOR_OUT}" 2>&1 &
EDITOR_PID=$!

log_info "waiting for the editor to report a ready engine (max ${READY_TIMEOUT}s)"

NEW_LOG=""
READY=0
elapsed=0
while [ "${elapsed}" -lt "${READY_TIMEOUT}" ]; do
    if ! kill -0 "${EDITOR_PID}" 2>/dev/null; then
        wait "${EDITOR_PID}" 2>/dev/null
        break
    fi

    NEW_LOG="$(ls -1t "${LOG_DIR}"/*.log 2>/dev/null | head -1)"

    if [ -n "${NEW_LOG}" ] && grep -q "All systems initialized successfully" "${NEW_LOG}" 2>/dev/null; then
        READY=1
        break
    fi

    sleep 1
    elapsed=$((elapsed + 1))
done

if [ "${READY}" -ne 1 ]; then
    log_error "the editor never reached 'All systems initialized successfully'"
    [ -n "${NEW_LOG}" ] && tail -20 "${NEW_LOG}" | sed 's/^/    /' >&2
    tail -20 "${EDITOR_OUT}" | sed 's/^/    /' >&2
    exit 1
fi

log_info "engine ready after ~${elapsed}s; waiting up to ${FRAME_TIMEOUT}s for the first presented frame"

FRAME_OK=0
frame_elapsed=0
while [ "${frame_elapsed}" -lt "${FRAME_TIMEOUT}" ]; do
    if [ -n "${NEW_LOG}" ] && grep -q "First frame presented" "${NEW_LOG}" 2>/dev/null; then
        FRAME_OK=1
        break
    fi
    if ! kill -0 "${EDITOR_PID}" 2>/dev/null; then
        break
    fi
    sleep 1
    frame_elapsed=$((frame_elapsed + 1))
done

if [ "${FRAME_OK}" -eq 1 ]; then
    log_info "first frame presented after ~${frame_elapsed}s"
else
    log_warn "no frame presented within ${FRAME_TIMEOUT}s"
    log_warn "  On a software rasteriser (Mesa llvmpipe) the editor can take minutes per"
    log_warn "  frame, and in practice never presents at all - that is a property of the"
    log_warn "  renderer running without a GPU, not a regression. Use --require-frame on"
    log_warn "  machines with a real Vulkan device to make this fatal."
    if [ "${REQUIRE_FRAME}" -eq 1 ]; then
        log_error "--require-frame was given but no frame was presented"
        [ -n "${NEW_LOG}" ] && tail -15 "${NEW_LOG}" | sed 's/^/    /' >&2
        exit 1
    fi
fi

log_info "keeping the editor alive for ${RUN_SECONDS}s"

survived=1
for _ in $(seq 1 "${RUN_SECONDS}"); do
    if ! kill -0 "${EDITOR_PID}" 2>/dev/null; then
        survived=0
        break
    fi
    sleep 1
done

# ------------------------------------------------------------------- verdict
if [ "${survived}" -ne 1 ]; then
    wait "${EDITOR_PID}" 2>/dev/null
    status=$?
    log_error "the editor died after the engine came up (status ${status})"

    if grep -qi "Segmentation fault\|terminate called\|Aborted" "${EDITOR_OUT}" 2>/dev/null; then
        grep -i -A5 "Segmentation fault\|terminate called\|Aborted" "${EDITOR_OUT}" | head -30 | sed 's/^/    /' >&2
    fi
    [ -n "${NEW_LOG}" ] && tail -15 "${NEW_LOG}" | sed 's/^/    /' >&2
    exit 1
fi

if [ "${FRAME_OK}" -eq 1 ]; then
    log_info "PASS - editor stayed up for ${RUN_SECONDS}s and presented a frame (log: ${NEW_LOG:-<none>})"
else
    log_info "PASS (liveness only) - editor stayed up for ${RUN_SECONDS}s, no frame presented (log: ${NEW_LOG:-<none>})"
fi
exit 0
