#!/usr/bin/env python3
# =============================================================================
# rg_ab_compare.py — compare the in-frame render-graph A/B capture written by
# DX12RenderGraphAbCapture (ZENGINE_RG_AB_CAPTURE=<frame>).
#
# Pairs every "<stage>.handwritten.<target>" image with "<stage>.graph.<target>"
# and reports, per pair: identical or not, how many pixels differ beyond the
# tolerance, the largest per-channel difference, and the bounding box of the
# differing region. Optionally writes PNGs (both runs + an amplified diff).
#
# Both runs render the SAME frame from the SAME inputs, so a correct graph path
# is expected to be bit-identical; the default tolerance is therefore 0.
#
# Exit code:  0  every pair within tolerance AND every stage really used the graph
#             1  at least one pair differs beyond tolerance
#             2  the capture cannot prove anything (missing / unreadable images,
#                write errors, or a "graph" run that fell back to the hand-written
#                path -- then A == B trivially, which must not count as a pass)
#
# Standard library only (no numpy): rows are compared as raw bytes first and
# only differing rows are decoded, so an identical 1080p capture takes well
# under a second.
#
# USAGE:
#   python scripts/rg_ab_compare.py <capture_dir> [--tolerance 0] [--png] [--out <dir>] [--json <file>]
# =============================================================================
import argparse
import json
import math
import os
import struct
import sys
import zlib

MAGIC = b'ZCAP'
HEADER_BYTES = 52


class Capture:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < HEADER_BYTES or data[:4] != MAGIC:
            raise ValueError('%s: not a .zcap file' % path)
        version, width, height, bpp = struct.unpack_from('<4I', data, 4)
        if version != 1:
            raise ValueError('%s: unsupported zcap version %d' % (path, version))
        self.format = data[20:52].split(b'\0', 1)[0].decode('ascii')
        self.width, self.height, self.bpp = width, height, bpp
        self.row_bytes = width * bpp
        self.pixels = memoryview(data)[HEADER_BYTES:]
        expected = self.row_bytes * height
        if len(self.pixels) != expected:
            raise ValueError('%s: %d pixel bytes, expected %d' % (path, len(self.pixels), expected))
        if self.format == 'R16G16B16A16_SFLOAT':
            if bpp != 8:
                raise ValueError('%s: RGBA16F with bpp %d' % (path, bpp))
            self.kind = 'half'
        elif self.format in ('R8G8B8A8_UNORM', 'R8G8B8A8_SRGB', 'B8G8R8A8_UNORM', 'B8G8R8A8_SRGB'):
            if bpp != 4:
                raise ValueError('%s: 8-bit format with bpp %d' % (path, bpp))
            self.kind = 'u8'
        else:
            raise ValueError('%s: unknown format %s' % (path, self.format))

    def row(self, y):
        return self.pixels[y * self.row_bytes:(y + 1) * self.row_bytes]

    def decode_row(self, y):
        """Per-channel values of row y as floats (8-bit normalised to 0..1)."""
        raw = self.row(y)
        if self.kind == 'half':
            return struct.unpack('<%de' % (self.width * 4), raw)
        return [b / 255.0 for b in raw]

    def is_bgra(self):
        return self.format.startswith('B8G8R8A8')


def channel_diff(a, b):
    if a == b:
        return 0.0
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
        return math.inf
    return abs(a - b)


def compare(a, b, tolerance):
    result = {
        'width': a.width, 'height': a.height, 'format': a.format,
        'identical': True, 'differing_pixels': 0, 'max_channel_diff': 0.0,
        'mean_abs_diff_of_differing': 0.0, 'bbox': None, 'differing_rows': 0,
    }
    if (a.width, a.height, a.format) != (b.width, b.height, b.format):
        result['identical'] = False
        result['error'] = 'size/format mismatch: %dx%d %s vs %dx%d %s' % (
            a.width, a.height, a.format, b.width, b.height, b.format)
        return result, None

    diff_rows = {}
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    total = 0.0
    for y in range(a.height):
        if a.row(y) == b.row(y):
            continue
        result['identical'] = False
        result['differing_rows'] += 1
        raw_a, raw_b = a.row(y), b.row(y)
        ra, rb = a.decode_row(y), b.decode_row(y)
        row_diff = [0.0] * a.width
        bpp = a.bpp
        for x in range(a.width):
            # Bitwise-equal pixels are equal -- including identical NaN payloads, which a
            # float comparison would wrongly report as different (NaN != NaN).
            if raw_a[x * bpp:(x + 1) * bpp] == raw_b[x * bpp:(x + 1) * bpp]:
                continue
            i = x * 4
            d = max(channel_diff(ra[i], rb[i]), channel_diff(ra[i + 1], rb[i + 1]),
                    channel_diff(ra[i + 2], rb[i + 2]), channel_diff(ra[i + 3], rb[i + 3]))
            if d == 0.0:
                continue
            row_diff[x] = d
            if d > result['max_channel_diff']:
                result['max_channel_diff'] = d
            if d > tolerance:
                result['differing_pixels'] += 1
                total += d if not math.isinf(d) else 0.0
                x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
        diff_rows[y] = row_diff
    if result['differing_pixels']:
        result['bbox'] = [x0, y0, x1, y1]
        result['mean_abs_diff_of_differing'] = total / result['differing_pixels']
    return result, diff_rows


# ----------------------------------------------------------------------------- PNG output
def write_png(path, width, height, rgb_rows):
    """rgb_rows: iterable of bytes objects, width*3 each."""
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)
        raw.extend(row)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(chunk(b'IEND', b''))


def preview_rows(cap):
    """Displayable 8-bit RGB rows. HDR is Reinhard-tonemapped + gamma 2.2."""
    for y in range(cap.height):
        if cap.kind == 'u8':
            raw = cap.row(y)
            out = bytearray(cap.width * 3)
            for x in range(cap.width):
                i = x * 4
                r, g, b = (raw[i + 2], raw[i + 1], raw[i]) if cap.is_bgra() else (raw[i], raw[i + 1], raw[i + 2])
                out[x * 3:x * 3 + 3] = bytes((r, g, b))
            yield bytes(out)
        else:
            values = cap.decode_row(y)
            out = bytearray(cap.width * 3)
            for x in range(cap.width):
                for c in range(3):
                    v = values[x * 4 + c]
                    if not math.isfinite(v) or v < 0.0:
                        v = 0.0
                    v = (v / (1.0 + v)) ** (1.0 / 2.2)
                    out[x * 3 + c] = min(255, int(v * 255.0 + 0.5))
            yield bytes(out)


def diff_rows_png(width, height, diff_rows, max_diff):
    scale = 0.0 if max_diff <= 0.0 or math.isinf(max_diff) else 255.0 / max_diff
    for y in range(height):
        row = diff_rows.get(y)
        out = bytearray(width * 3)
        if row is not None:
            for x, d in enumerate(row):
                if d > 0.0:
                    level = 255 if (math.isinf(d) or scale == 0.0) else max(64, min(255, int(d * scale)))
                    out[x * 3] = level  # differing pixels in red, at least clearly visible
        yield bytes(out)


# ----------------------------------------------------------------------------- driver
def main(argv=None):
    parser = argparse.ArgumentParser(description='Compare a render-graph A/B capture.')
    parser.add_argument('capture_dir')
    parser.add_argument('--tolerance', type=float, default=0.0,
                        help='per-channel absolute difference still counted as equal (default 0 = bit-exact)')
    parser.add_argument('--png', action='store_true', help='write handwritten/graph previews and a diff image')
    parser.add_argument('--out', default=None, help='where to put PNGs (default: <capture_dir>/compare)')
    parser.add_argument('--json', default=None, help='also write the report as JSON to this file')
    args = parser.parse_args(argv)

    manifest_path = os.path.join(args.capture_dir, 'manifest.json')
    try:
        with open(manifest_path, encoding='utf-8') as f:
            manifest = json.load(f)
    except (OSError, ValueError) as e:
        print('ERROR: cannot read %s: %s' % (manifest_path, e))
        return 2

    problems = []
    for stage in manifest.get('stages', []):
        if not stage.get('attempted'):
            problems.append('stage %s was not attempted' % stage.get('stage'))
        elif not stage.get('graph_used'):
            problems.append('stage %s: the "graph" run fell back to the hand-written path -- '
                            'A == B trivially, the comparison proves nothing' % stage.get('stage'))
    images = {}
    for image in manifest.get('images', []):
        if not image.get('written'):
            problems.append('image %s not written: %s' % (image.get('name'), image.get('error') or 'unknown'))
            continue
        images[image['name']] = image

    pairs = []
    for name in sorted(images):
        parts = name.split('.')
        if len(parts) >= 3 and parts[1] == 'handwritten':
            other = '.'.join([parts[0], 'graph'] + parts[2:])
            if other in images:
                pairs.append((name, other))
            else:
                problems.append('no graph counterpart for %s' % name)
    if not pairs:
        problems.append('no handwritten/graph image pairs in the manifest')

    print('render-graph A/B capture: %s (frame %s)' % (args.capture_dir, manifest.get('frame')))
    for note in manifest.get('notes', []):
        print('  note: %s' % note)

    report = {'capture_dir': args.capture_dir, 'frame': manifest.get('frame'),
              'tolerance': args.tolerance, 'pairs': [], 'problems': problems}
    any_diff = False
    out_dir = args.out or os.path.join(args.capture_dir, 'compare')
    for hand_name, graph_name in pairs:
        try:
            a = Capture(os.path.join(args.capture_dir, images[hand_name]['file']))
            b = Capture(os.path.join(args.capture_dir, images[graph_name]['file']))
        except (OSError, ValueError) as e:
            problems.append(str(e))
            continue
        result, diff_rows = compare(a, b, args.tolerance)
        target = hand_name.replace('.handwritten', '')
        result['target'] = target
        report['pairs'].append(result)
        if 'error' in result:
            problems.append('%s: %s' % (target, result['error']))
            continue
        within = result['differing_pixels'] == 0
        any_diff = any_diff or not within
        if result['identical']:
            verdict = 'IDENTICAL'
        elif within:
            verdict = 'WITHIN TOLERANCE (max %.6g)' % result['max_channel_diff']
        else:
            verdict = 'DIFFERS: %d px > %g, max %.6g, bbox %s' % (
                result['differing_pixels'], args.tolerance, result['max_channel_diff'], result['bbox'])
        print('  %-28s %dx%d %-20s %s' % (target, a.width, a.height, a.format, verdict))

        if args.png:
            os.makedirs(out_dir, exist_ok=True)
            write_png(os.path.join(out_dir, target + '.handwritten.png'), a.width, a.height, preview_rows(a))
            write_png(os.path.join(out_dir, target + '.graph.png'), b.width, b.height, preview_rows(b))
            write_png(os.path.join(out_dir, target + '.diff.png'), a.width, a.height,
                      diff_rows_png(a.width, a.height, diff_rows or {}, result['max_channel_diff']))

    for problem in problems:
        print('  PROBLEM: %s' % problem)
    if args.png and pairs:
        print('  PNGs: %s' % out_dir)

    if problems:
        verdict, code = 'INCONCLUSIVE', 2
    elif any_diff:
        verdict, code = 'FAIL', 1
    else:
        verdict, code = 'PASS', 0
    report['verdict'] = verdict
    print('RESULT: %s' % verdict)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, default=lambda v: None if isinstance(v, float) and math.isinf(v) else v)
    return code


if __name__ == '__main__':
    sys.exit(main())
