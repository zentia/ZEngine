#!/usr/bin/env python3
"""Build and run a ZRuntime-linked smoke test headless under node (web build).

The engine's smoke tests are standalone main() executables that link ZRuntime
(--whole-archive). On Windows the native toolchain is not usable from the sandbox,
but build-web/ (emcc + ninja) builds the same ZRuntime as a wasm static library.
This script reuses that build:

  1. asks ninja for the exact compile + link command of an existing web smoke-test
     target (the "template"; default NaniteClusterBuilderSmokeTest, which is always
     configured in build-web),
  2. compiles the requested test .cpp with the template's compile flags (same
     defines / includes / PCH settings ZRuntime was built with),
  3. links it against bin/libZRuntime.a with the template's link line plus the flags
     the CMake smoke-test targets lack on the web: WebGL2 + emmalloc (the library's
     WebGL2RHI / mimalloc need them) and node + NODERAWFS so the test reads and writes
     the real file system,
  4. runs it with node in a scratch working directory and returns its exit code.

Usage:
  python scripts/web_smoke_run.py <test.cpp> [--name N] [--rebuild-lib] [-- test args...]

Exit code: the test's exit code (0 = pass); 3 = build failure.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BUILD = os.path.join(ROOT, 'build-web')
NINJA = r'C:\emsdk\ninja\1.13.2_64bit\ninja.exe'
TEMPLATE_TARGET = 'engine/Source/Runtime/Function/Render/Nanite/Test/NaniteClusterBuilderSmokeTest.js'
TEMPLATE_SOURCE = 'Function/Render/Nanite/Test/NaniteClusterBuilderSmokeTest.cpp'
TEMPLATE_OBJECT = 'NaniteClusterBuilderSmokeTest.cpp.o'

NODE_LINK_FLAGS = (
    ' -sUSE_WEBGL2=1 -sFULL_ES3=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2'
    ' -sMALLOC=emmalloc -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=2097152'
    ' -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sASSERTIONS=1'
    ' -sNO_DISABLE_EXCEPTION_CATCHING -sALLOW_UNIMPLEMENTED_SYSCALLS=1'
)


def run(cmd, cwd, **kw):
    return subprocess.run(cmd, cwd=cwd, shell=True, **kw)


def template_commands():
    out = subprocess.run([NINJA, '-t', 'commands', TEMPLATE_TARGET], cwd=BUILD,
                         capture_output=True, text=True, check=True).stdout.splitlines()
    compile_line = next(l for l in out if ' -c ' in l and l.replace('\\', '/').find(TEMPLATE_SOURCE) >= 0)
    link_line = next(l for l in out if TEMPLATE_OBJECT in l and '--whole-archive' in l and ' -c ' not in l)
    return compile_line, link_line


def unwrap_cmd(line):
    """`cmd.exe /C "cd . && X && cd ."` -> X (ninja wraps link steps that way)."""
    m = re.match(r'^.*?cmd\.exe /C "cd \. && (.*?)(?: && cd \.)?"\s*$', line)
    return m.group(1) if m else line


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source')
    ap.add_argument('--name')
    ap.add_argument('--extra', action='append', default=[],
                    help='additional .cpp compiled into the test (e.g. Editor TUs that are not in libZRuntime)')
    ap.add_argument('--rebuild-lib', action='store_true', help='bring bin/libZRuntime.a up to date first')
    argv = sys.argv[1:]
    test_args = []
    if '--' in argv:
        i = argv.index('--')
        argv, test_args = argv[:i], argv[i + 1:]
    a = ap.parse_args(argv)

    source = os.path.abspath(a.source).replace('\\', '/')
    name = a.name or os.path.splitext(os.path.basename(source))[0]
    work = os.path.join(BUILD, 'smoke_node', name)
    os.makedirs(work, exist_ok=True)
    obj = os.path.join(work, name + '.o').replace('\\', '/')
    js = os.path.join(work, name + '.js').replace('\\', '/')

    if a.rebuild_lib:
        r = run(f'"{NINJA}" libZRuntime.a', BUILD)
        if r.returncode != 0:
            return 3

    compile_line, link_line = template_commands()

    # Compile: swap the template's source and object for ours (plus any --extra TUs).
    objects = []
    for i, src in enumerate([source] + [os.path.abspath(x).replace(os.sep, '/') for x in a.extra]):
        o = obj if i == 0 else os.path.join(work, '%s.extra%d.o' % (name, i)).replace(os.sep, '/')
        cc = re.sub(r' -c \S*' + re.escape(TEMPLATE_SOURCE.split('/')[-1]), lambda _m: ' -c ' + src, compile_line, count=1)
        cc = re.sub(r' -o \S+' + re.escape(TEMPLATE_OBJECT), lambda _m: ' -o ' + o, cc, count=1)
        if src not in cc or o not in cc:
            print('web_smoke_run: could not rewrite the template compile command', file=sys.stderr)
            return 3
        print(f'==> compiling {os.path.relpath(src, ROOT)}')
        if run(cc, BUILD).returncode != 0:
            return 3
        objects.append(o)

    # Link: our object, our output, node flags.
    ld = unwrap_cmd(link_line)
    ld = re.sub(r'\S+' + re.escape(TEMPLATE_OBJECT), lambda _m: ' '.join(objects), ld, count=1)
    ld = re.sub(r' -o \S+\.js', ' -o ' + js, ld, count=1)
    # Emscripten's getenv() sees its own ENV table, not node's: there is no TMPDIR, so
    # std::filesystem::temp_directory_path() falls back to "/tmp", which does not exist
    # on a Windows host under NODERAWFS. Point it at the scratch working directory.
    pre_js = os.path.join(work, 'pre.js').replace(os.sep, '/')
    with open(pre_js, 'w') as f:
        f.write("Module['preRun'] = (Module['preRun'] || []).concat([function () { ENV['TMPDIR'] = '.'; }]);\n")
    ld = ld.replace(' -rdynamic', ' -rdynamic' + NODE_LINK_FLAGS + ' --pre-js ' + pre_js, 1)
    print(f'==> linking {name}.js (ZRuntime --whole-archive)')
    r = run(ld, BUILD, capture_output=True, text=True)
    if r.returncode != 0:
        errors = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l]
        for l in errors[:40]:
            print('  ' + l[:400])
        return 3

    print(f'==> running {name}')
    return subprocess.run(['node', js] + test_args, cwd=work).returncode


if __name__ == '__main__':
    sys.exit(main())
