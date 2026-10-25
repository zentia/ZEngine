#!/usr/bin/env bash
# =============================================================================
# rg_ab_selftest.sh — headless end-to-end check of the render-graph A/B capture
# tooling: the ENGINE's .zcap/manifest encoder (RenderGraphAbCaptureFormat.h,
# the code DX12RenderGraphAbCapture runs) against scripts/rg_ab_compare.py.
#
#  1. emcc-compiles Passes/Test/RenderGraphAbCaptureFormatTest.cpp (NODERAWFS,
#     so node writes real files) and runs it: synthetic captures laid out with
#     the real DX12 256-byte readback row pitch and garbage in the padding.
#  2. Runs the compare script on each scenario and checks its exit code
#     (0 PASS / 1 FAIL / 2 INCONCLUSIVE) and the reported numbers.
#  3. Decodes known pixels back in Python to prove the padding was stripped and
#     the half floats round-trip.
#
# What it does NOT cover: the GPU half (CmdCopyImageToBuffer, readback timing,
# restoring layouts). That needs the DX12 editor run.
#
# USAGE (repo root):  bash scripts/rg_ab_selftest.sh
# REQUIRES: emsdk at C:/emsdk, node, python 3.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"
source /c/emsdk/emsdk_env.sh >/dev/null 2>&1

OUT="${REPO_ROOT}/build-web/rg_ab_selftest"
rm -rf "${OUT}"
mkdir -p "${OUT}/captures"

echo "==> [1/3] build + run the capture writer (engine encoder)"
emcc -std=c++23 -O1 -I engine/Source \
  engine/Source/Runtime/Function/Render/Passes/Test/RenderGraphAbCaptureFormatTest.cpp \
  -s NODERAWFS=1 -s ASSERTIONS=1 -o "${OUT}/writer.js"
node "${OUT}/writer.js" "${OUT}/captures"

FAILS=0
expect() {  # expect <scenario> <expected exit code> [extra compare args...]
    local scenario="$1" want="$2"; shift 2
    local got=0
    local tag="${scenario}${1:+_}$(printf '%s' "$*" | tr -c 'A-Za-z0-9.' '_')"
    python scripts/rg_ab_compare.py "${OUT}/captures/${scenario}" --json "${OUT}/${tag}.json" "$@" \
        > "${OUT}/${tag}.log" 2>&1 || got=$?
    if [ "${got}" -eq "${want}" ]; then
        echo "    ok   ${scenario} $* -> exit ${got} ($(tail -1 "${OUT}/${tag}.log"))"
    else
        echo "    FAIL ${scenario} $* -> exit ${got}, expected ${want}"
        sed 's/^/         /' "${OUT}/${tag}.log"
        FAILS=$((FAILS + 1))
    fi
}

echo "==> [2/3] compare verdicts"
expect identical 0
expect identical 0 --png
expect one_pixel 1
expect tiny_diff 1
expect tiny_diff 0 --tolerance 0.01
expect fell_back 2
expect missing 2

echo "==> [3/3] reported numbers and decoded pixels"
python - "${OUT}" <<'PY' || FAILS=$((FAILS + 1))
import json, os, struct, sys
sys.path.insert(0, 'scripts')
import rg_ab_compare as c

out = sys.argv[1]
fails = []
def check(cond, what):
    print('    %s %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails.append(what)

r = json.load(open(os.path.join(out, 'one_pixel.json')))
rp1 = [p for p in r['pairs'] if p['target'] == 'rp1.backup_odd'][0]
check(rp1['differing_pixels'] == 1, 'one_pixel: exactly 1 differing pixel')
check(rp1['bbox'] == [123, 45, 123, 45], 'one_pixel: bbox is (123,45)')
check(abs(rp1['max_channel_diff'] - (999.0 - 123.0)) < 1e-6, 'one_pixel: max diff = 999 - 123')
others = [p for p in r['pairs'] if p['target'] != 'rp1.backup_odd']
check(len(others) == 2 and all(p['identical'] for p in others), 'one_pixel: the other two pairs identical')

r = json.load(open(os.path.join(out, 'tiny_diff.json')))
rp1 = [p for p in r['pairs'] if p['target'] == 'rp1.backup_odd'][0]
check(rp1['differing_pixels'] == 8 and rp1['bbox'] == [20, 10, 23, 11], 'tiny_diff: 4x2 block at (20..23, 10..11)')
check(abs(rp1['max_channel_diff'] - 1.0 / 1024.0) < 1e-7, 'tiny_diff: max diff = 1/1024')

hdr = c.Capture(os.path.join(out, 'captures', 'pattern', 'rp1.handwritten.backup_odd.zcap'))
ldr = c.Capture(os.path.join(out, 'captures', 'pattern', 'rp2.handwritten.backbuffer.zcap'))
check((hdr.width, hdr.height, hdr.bpp, hdr.format) == (300, 70, 8, 'R16G16B16A16_SFLOAT'), 'header round-trip (HDR)')
check((ldr.width, ldr.height, ldr.bpp, ldr.format) == (300, 70, 4, 'R8G8B8A8_UNORM'), 'header round-trip (LDR)')
row = hdr.decode_row(33)
check(row[299 * 4:299 * 4 + 4] == (299.0, 16.5, 0.25, 1.0), 'HDR pixel (299,33) = (299, 16.5, 0.25, 1)')
check(bytes(ldr.row(69)[4 * 255:4 * 256]) == bytes((255, 69, 7, 255)), 'LDR pixel (255,69) = (255,69,7,255)')
# Row padding (0xCD in the GPU-shaped source) must be stripped: every row starts with the
# x=0 pixel and ends with the x=299 pixel. (A blanket "no 0xCD byte" check would be wrong:
# x=205 is a legitimate 0xCD red value.)
check(all(bytes(ldr.row(y)[:4]) == bytes((0, y, 7, 255)) and
          bytes(ldr.row(y)[-4:]) == bytes((299 & 0xFF, y, 7, 255)) for y in range(ldr.height)),
      'LDR rows tightly packed (padding stripped; first/last pixel of every row exact)')
check(all(tuple(hdr.decode_row(y)[-4:]) == (299.0, y * 0.5, 0.25, 1.0) for y in range(hdr.height)),
      'HDR rows tightly packed (last pixel of every row exact)')

m = json.load(open(os.path.join(out, 'captures', 'pattern', 'manifest.json')))
check(m['frame'] == 120 and m['notes'][1] == 'quote " and \\ escaped', 'manifest JSON parses, escaping round-trips')
png = os.path.join(out, 'captures', 'identical', 'compare', 'rp1.backup_odd.diff.png')
check(os.path.exists(png) and open(png, 'rb').read(8) == b'\x89PNG\r\n\x1a\n', '--png wrote a valid PNG')

# NaN: a pixel holding the SAME NaN bits in both runs is equal (NaN != NaN as floats);
# a real difference elsewhere in that row must still be found, and only that one.
d = os.path.join(out, 'nan_same_row'); os.makedirs(d)
w, h = 4, 2
def zcap(name, halfs):
    open(os.path.join(d, name + '.zcap'), 'wb').write(
        b'ZCAP' + struct.pack('<4I', 1, w, h, 8) + b'R16G16B16A16_SFLOAT'.ljust(32, b'\0') +
        struct.pack('<%dH' % len(halfs), *halfs))
base = [0x3C00] * (w * h * 4); base[5] = 0x7E00      # (1,0).G = quiet NaN in both runs
graph = list(base); graph[1] = 0x4000                # (0,0).G 1.0 -> 2.0, same row as the NaN
zcap('rp1.handwritten.x', base); zcap('rp1.graph.x', graph)
img = lambda n: {'name': n, 'file': n + '.zcap', 'width': w, 'height': h,
                 'format': 'R16G16B16A16_SFLOAT', 'written': True, 'error': ''}
json.dump({'frame': 1, 'stages': [{'stage': 'rp1', 'attempted': True, 'graph_used': True}],
           'images': [img('rp1.handwritten.x'), img('rp1.graph.x')], 'notes': []},
          open(os.path.join(d, 'manifest.json'), 'w'))
import contextlib, io
with contextlib.redirect_stdout(io.StringIO()):
    code = c.main([d, '--json', os.path.join(d, 'r.json')])
p = json.load(open(os.path.join(d, 'r.json')))['pairs'][0]
check(code == 1 and p['differing_pixels'] == 1 and p['bbox'] == [0, 0, 0, 0] and p['max_channel_diff'] == 1.0,
      'identical NaN bits are equal; the one real difference in that row is still found')
sys.exit(1 if fails else 0)
PY

if [ "${FAILS}" -ne 0 ]; then
    echo "==> rg_ab self-test: FAIL (${FAILS})"
    exit 1
fi
echo "==> rg_ab self-test: PASS"
