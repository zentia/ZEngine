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
# --threads: the pthreads build. Configure once (output lands in bin/web-mt/, so it
# does not overwrite build-web's bin/libZRuntime.a):
#   emcmake cmake -S . -B build-web-mt -G Ninja -DCMAKE_BUILD_TYPE=Debug
#       -DZENGINE_OUTPUT_SUBDIR=web-mt -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread
#       -DCMAKE_EXE_LINKER_FLAGS=-pthread -DZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON
#       -DZENGINE_BUILD_DATATABLE_COOK=ON
#   ninja -C build-web-mt libZRuntime.a
# Every object linked into a shared-memory wasm must be built with -pthread (atomics +
# bulk-memory), which is why this is a separate build tree rather than a link flag.
# NOTE: configuring either tree regenerates engine_log.h, which invalidates the OTHER
# tree's PCH -- run `ninja ZEnginePCH libZRuntime.a` there before its next test run.
BUILD_MT = os.path.join(ROOT, 'build-web-mt')
THREAD_LINK_FLAGS = ' -pthread -sPTHREAD_POOL_SIZE=8 -sPROXY_TO_PTHREAD=1'
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
    ap.add_argument('--include-dir', action='append', default=[],
                    help='extra -I directory (what the CMake target adds with target_include_directories)')
    ap.add_argument('--define', action='append', default=[],
                    help='extra -D NAME=VALUE (what the CMake target adds with target_compile_definitions)')
    ap.add_argument('--threads', action='store_true',
                    help='build and run against the pthreads tree (build-web-mt, bin/web-mt/libZRuntime.a)')
    ap.add_argument('--link', action='append', default=[],
                    help='extra static library to link (e.g. bin/libbc7enc.a)')
    argv = sys.argv[1:]
    test_args = []
    if '--' in argv:
        i = argv.index('--')
        argv, test_args = argv[:i], argv[i + 1:]
    a = ap.parse_args(argv)
    global BUILD
    if a.threads:
        BUILD = BUILD_MT
        if not os.path.isfile(os.path.join(BUILD, 'build.ninja')):
            print('web_smoke_run: --threads needs the configured build-web-mt tree (see the comment at BUILD_MT)',
                  file=sys.stderr)
            return 3

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
        extra_flags = ''.join(' -I"%s"' % os.path.abspath(d).replace(os.sep, '/') for d in a.include_dir)
        extra_flags += ''.join(' "-D%s"' % d.replace('"', '\\"') for d in a.define)
        if extra_flags:
            cc = cc.replace(' -c ' + src, extra_flags + ' -c ' + src, 1)
        print(f'==> compiling {os.path.relpath(src, ROOT)}')
        if run(cc, BUILD).returncode != 0:
            return 3
        objects.append(o)

    # Link: our object, our output, node flags.
    ld = unwrap_cmd(link_line)
    ld = re.sub(r'\S+' + re.escape(TEMPLATE_OBJECT), lambda _m: ' '.join(objects), ld, count=1)
    ld = re.sub(r' -o \S+\.js', ' -o ' + js, ld, count=1)
    if a.link:
        libs = ' '.join('"%s"' % os.path.abspath(l).replace(os.sep, '/') for l in a.link)
        ld = ld.replace(' -o ' + js, ' ' + libs + ' -o ' + js, 1)
    # Emscripten's getenv() sees its own ENV table, not node's: there is no TMPDIR, so
    # std::filesystem::temp_directory_path() falls back to "/tmp", which does not exist
    # on a Windows host under NODERAWFS. Point it at the scratch working directory.
    #
    # NODERAWFS also returns node's process.cwd() verbatim; on Windows that is
    # "E:<backslash>x<backslash>y", which the wasm side's POSIX path code cannot use:
    # getcwd() fails with ENOENT, so std::filesystem::current_path() / absolute() throw
    # (AssetBundle / ProjectInfoPaths smoke tests died on this). Present the cwd as
    # "/x/y" instead: absolute for the wasm side, and node on Windows resolves a
    # drive-less "/x/y" against the current drive, i.e. the same directory.
    pre_js = os.path.join(work, 'pre.js').replace(os.sep, '/')
    with open(pre_js, 'w') as f:
        f.write("Module['preRun'] = (Module['preRun'] || []).concat([function () {\n"
                "  ENV['TMPDIR'] = '.';\n"
                "  if (typeof process !== 'undefined' && process.platform === 'win32' && FS.cwd) {\n"
                "    var raw = FS.cwd, sep = String.fromCharCode(92);\n"
                "    FS.cwd = function () {\n"
                "      var p = raw.apply(this, arguments);\n"
                "      if (typeof p !== 'string') return p;\n"
                "      p = p.split(sep).join('/');\n"
                "      return /^[A-Za-z]:[/]/.test(p) ? p.slice(2) : p;\n"
                "    };\n"
                "  }\n"
                "}]);\n")
    link_flags = NODE_LINK_FLAGS + (THREAD_LINK_FLAGS if a.threads else '')
    ld = ld.replace(' -rdynamic', ' -rdynamic' + link_flags + ' --pre-js ' + pre_js, 1)
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
