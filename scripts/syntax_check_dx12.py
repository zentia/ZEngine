#!/usr/bin/env python3
# =============================================================================
# syntax_check_dx12.py — compile-check (clang-cl /Zs, no codegen, no link)
# Windows/DX12 translation units with the EXACT include paths and defines of
# the ZRuntime Debug|x64 project in build/ (VS 17 2022 generator), plus the
# CommonPCH header force-included the way the real build uses it.
#
# WHY: this machine has no usable MSBuild from the agent sandbox, but clang-cl,
# the MSVC STL headers and the Windows SDK are installed. /Zs runs the full
# front end (templates instantiated, overloads resolved), so a TU that passes
# here type-checks under the same headers MSVC sees. It is NOT a substitute for
# an MSVC build: MSVC-specific diagnostics and link errors are not covered.
#
# A negative control matters: a planted typo must fail. That was checked when
# this script was added (step 2.4 DrawRP1WithGraph wiring).
#
# USAGE (repo root):
#   python scripts/syntax_check_dx12.py engine/Source/Runtime/Function/Render/Passes/DX12MainCameraPass.cpp [...]
#   python scripts/syntax_check_dx12.py --include engine/Libraries/ZAppShell/include engine/Source/Game/ZGame.cpp
#     (--include DIR, repeatable: extra include dirs for TUs outside ZRuntime,
#      e.g. the editor / game entry points)
#
# REQUIRES: build/ configured with the VS generator (ZRuntime.vcxproj present),
#           VS 2022 with the "C++ Clang tools" component, Windows 10 SDK.
# =============================================================================
import glob
import io
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
VCXPROJ = os.path.join(ROOT, 'build', 'engine', 'Source', 'Runtime', 'ZRuntime.vcxproj')


def latest(pattern):
    hits = sorted(glob.glob(pattern))
    if not hits:
        sys.exit('not found: ' + pattern)
    return hits[-1]


def main(argv):
    files, extra_inc = [], []
    it = iter(argv)
    for a in it:
        if a == '--include':
            extra_inc.append(os.path.abspath(next(it)))
        else:
            files.append(a)
    if not files:
        sys.exit(__doc__ or 'usage: syntax_check_dx12.py <file.cpp>...')
    s = io.open(VCXPROJ, encoding='utf-8', errors='ignore').read()
    m = re.search(r"<ItemDefinitionGroup Condition=\"'\$\(Configuration\)\|\$\(Platform\)'=='Debug\|x64'\">(.*?)"
                  r"</ItemDefinitionGroup>", s, re.S)
    cl = re.search(r'<ClCompile>(.*?)</ClCompile>', m.group(1), re.S).group(1)

    def tag(t):
        x = re.search(r'<%s>(.*?)</%s>' % (t, t), cl, re.S)
        return x.group(1) if x else ''

    inc = [i for i in tag('AdditionalIncludeDirectories').split(';') if i and '%(' not in i]
    defs = [d for d in tag('PreprocessorDefinitions').split(';') if d and '%(' not in d]
    pch = tag('PrecompiledHeaderFile')

    vs = latest('C:/Program Files/Microsoft Visual Studio/2022/*')
    msvc = latest(vs + '/VC/Tools/MSVC/*')
    sdk = latest('C:/Program Files (x86)/Windows Kits/10/Include/10.*')
    clang_cl = vs + '/VC/Tools/Llvm/x64/bin/clang-cl.exe'

    args = [clang_cl, '/nologo', '/Zs', '/std:c++latest', '/EHsc', '/MDd', '/utf-8', '-Wno-everything', '/D_DEBUG']
    args += ['/D' + d for d in defs]
    args += ['/I' + i for i in inc + extra_inc]
    args += ['/imsvc' + p for p in (msvc + '/include', sdk + '/ucrt', sdk + '/um', sdk + '/shared', sdk + '/winrt')]
    if pch and os.path.exists(pch):
        args.append('/FI' + pch)

    rc = 0
    for f in files:
        path = os.path.abspath(f)
        print('==> clang-cl /Zs', os.path.relpath(path, ROOT), flush=True)
        p = subprocess.run(args + [path], capture_output=True, text=True)
        errors = [l for l in (p.stdout + p.stderr).splitlines() if 'error' in l]
        print('\n'.join(errors[:40]) if errors else '    no errors')
        rc |= p.returncode
    print('RESULT:', 'PASS' if rc == 0 else 'FAIL')
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
