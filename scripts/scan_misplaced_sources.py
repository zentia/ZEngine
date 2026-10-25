#!/usr/bin/env python3
"""Find source files whose contents were put in the wrong file.

The "init clean repo" import left several .cpp files holding a verbatim copy of
some OTHER header (Misc/InputEvent.cpp was TimelineDirector.h, Texture.cpp was
RenderTextureDesc.h, RHIThread.cpp was RHIDrawList.h, ...). They compile, so
nothing notices -- until the real header changes and the stale copy becomes a
second, different definition of the same class in one program (ODR violation),
which is what happened with TimelineDirector.

Checks (engine/Source by default, 3rdparty / Generated skipped):
  1. a .cpp/.mm whose contents equal (ignoring whitespace) a header's;
  2. a .cpp/.mm containing '#pragma once' (a header body living in a .cpp);
  3. --odr: classes / structs defined with a body in more than one file with
     different bodies (heuristic; nested-type out-of-line definitions and
     std:: specializations show up as noise, so this one is report-only).

Groups made only of .cpp/.mm files are ignored: those are deliberate per-platform
mirrors (e.g. Editor/Platform/Linux/EditorViewLinux.cpp == MacOS/EditorViewMacOS.cpp),
and only one of them is compiled per platform.

Exit code: 0 clean, 1 checks 1/2 found something.

    python scripts/scan_misplaced_sources.py [root] [--odr]
"""
import collections
import hashlib
import os
import re
import sys

SKIP = ('/3rdparty/', '/ThirdParty/', '/Generated/', '/node_modules/')
SOURCE_EXT = ('.cpp', '.mm')
HEADER_EXT = ('.h', '.hpp', '.inl')


def norm(text):
    return re.sub(r'\s+', '', text)


def collect(root):
    out = {}
    for d, _, fs in os.walk(root):
        dd = d.replace(os.sep, '/')
        if any(s in dd + '/' for s in SKIP):
            continue
        for f in fs:
            if f.endswith(SOURCE_EXT + HEADER_EXT):
                p = dd + '/' + f
                try:
                    out[p] = open(p, encoding='utf-8', errors='ignore').read()
                except OSError:
                    pass
    return out


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    root = args[0] if args else 'engine/Source'
    texts = collect(root)
    hits = 0

    groups = collections.defaultdict(list)
    for p, t in texts.items():
        n = norm(t)
        if len(n) > 200:
            groups[hashlib.sha1(n.encode()).hexdigest()].append(p)
    print('== 1. .cpp identical to a header ==')
    for ps in groups.values():
        if any(p.endswith(SOURCE_EXT) for p in ps) and any(p.endswith(HEADER_EXT) for p in ps):
            hits += 1
            print('   ' + '  ==  '.join(sorted(ps)))

    print('== 2. .cpp containing #pragma once ==')
    for p, t in sorted(texts.items()):
        if p.endswith(SOURCE_EXT) and re.search(r'^\s*#\s*pragma\s+once', t, re.M):
            hits += 1
            print('   ' + p)

    if '--odr' in argv:
        print('== 3. same class/struct name, different bodies (heuristic, report only) ==')
        defre = re.compile(r'^(?:template\s*<[^>]*>\s*)?(class|struct)\s+(?:[A-Z_]+_API\s+)?'
                           r'([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{;]*)?\{', re.M)
        defs = collections.defaultdict(list)
        for p, t in texts.items():
            for m in defre.finditer(t):
                i = j = m.end() - 1
                depth = 0
                while j < len(t):
                    if t[j] == '{':
                        depth += 1
                    elif t[j] == '}':
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                body = norm(t[i:j + 1])
                defs[m.group(2)].append((p, t.count('\n', 0, m.start()) + 1,
                                         hashlib.sha1(body.encode()).hexdigest(), len(body)))
        for name, ds in sorted(defs.items()):
            if len({d[0] for d in ds}) > 1 and len({d[2] for d in ds}) > 1 and all(d[3] > 80 for d in ds):
                print('   %s: %s' % (name, ', '.join('%s:%d' % (d[0], d[1]) for d in ds)))

    print('scan_misplaced_sources: %s' % ('clean' if hits == 0 else '%d finding(s)' % hits))
    return 0 if hits == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
