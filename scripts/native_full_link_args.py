#!/usr/bin/env python3
"""Print the --extra-source / --exclude-archive-member arguments that make a native
smoke-test binary see EVERY runtime type with CURRENT code.

Why this exists (found 2026-10-27 while chasing "P18/types only round-trips 9 types")
-----------------------------------------------------------------------------------
bin/libZRuntime.a is the output of the last cmake build -- days old on a dev box. Two
different things go wrong when a test iterates TypeManager::GetRuntimeTypes():

1. Most types never get registered at all. A type is registered by its *.gen.cpp
   (IMPLEMENT_REGISTER_CLASS -> RegisterKlass<T> -> GlobalRegisterType(desc)). The stale
   archive members register themselves with desc.type == nullptr, which RegisterType
   silently drops (`if (desc.type == nullptr) return;`): 61 of 71 registration functions
   ran and registered nothing, leaving 17 types (9 producible). Recompiling the gen.cpp
   against today's headers fixes the descriptor.
   NOT the cause (ruled out by experiment): the archive members not being linked.
   Forcing the whole archive in (-force_load) collected 71 registration functions and
   still registered the same 17.

2. Recompiling the gen.cpp is not enough: a class whose HEADER changed since the archive
   was built (a new virtual, e.g. SpineRenderer::OnOwnerActiveChanged) has a stale
   implementation .cpp.o in the archive, so the binary dies at startup with
   `dyld: symbol not found in flat namespace`. Every runtime .cpp changed since the
   archive was built (and that has a gen.cpp) has to be recompiled too.

Both lists are computed here instead of hand-maintained, because they rot the moment
anyone commits.

Usage:  python3 scripts/native_full_link_args.py   # prints shell-quoted args, one per line
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ARCHIVE = os.path.join(ROOT, "bin", "libZRuntime.a")
GEN_DIR = "build/Generated/ZHT/ZRuntime/Src"


def archive_members():
    r = subprocess.run(["ar", "-t", ARCHIVE], capture_output=True, text=True, check=True)
    return set(r.stdout.split())


def last_commit_time(path):
    r = subprocess.run(["git", "log", "-1", "--format=%ct", "--", path], cwd=ROOT, capture_output=True, text=True)
    return int(r.stdout.strip() or 0)


def main():
    members = archive_members()
    archive_mtime = os.stat(ARCHIVE).st_mtime

    gens = sorted(glob.glob(os.path.join(ROOT, GEN_DIR, "*.gen.cpp")))
    gen_stems = {os.path.basename(g)[: -len(".gen.cpp")] for g in gens}

    sources = []  # (relative path, archive member name or None)
    for g in gens:
        rel = os.path.relpath(g, ROOT)
        sources.append((rel, os.path.basename(g) + ".o"))

    tracked = subprocess.run(["git", "ls-files", "engine/Source/Runtime/*.cpp"], cwd=ROOT,
                             capture_output=True, text=True).stdout.split()
    for f in tracked:
        stem = os.path.basename(f)[: -len(".cpp")]
        if stem not in gen_stems:
            continue  # only classes that register a type matter here
        if last_commit_time(f) > archive_mtime:
            sources.append((f, os.path.basename(f) + ".o"))

    out = []
    for rel, member in sources:
        out += ["--extra-source", rel]
        if member in members:
            out += ["--exclude-archive-member", member]
    print("\n".join(out))
    sys.stderr.write("native_full_link_args: %d sources (%d gen.cpp + %d stale class sources)\n"
                     % (len(sources), len(gens), len(sources) - len(gens)))


if __name__ == "__main__":
    main()
