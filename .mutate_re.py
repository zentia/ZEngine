"""Mutation harness for the Resources slice (AX / AY checks).

Same contract as the other .mutate_*.py: every mutation must make the smoke
test go red. Restore ends with `touch` -- a fast mutate/build/restore round
trip can otherwise leave MSBuild believing a TU is up to date, and the
resulting crash has nothing to do with the mutation (see .mutate_pm.py).

Known blind spot (deliberately not in the list)
----------------------------------------------
`Resources::Load`'s cache hit and `AssetManager`'s own identity map
(`ObjectManager::IDToPointer(GetInstanceIDFromPathAndFileID(path, 1))`) return
the SAME pointer, so "was the second Load served by my cache or by the
engine's identity map" is not observable from outside. Only the *eviction*
half (GetLoadedCount going up and down) can be mutated visibly, which is why
R7 / R8 / R12 are about emplace / erase rather than about the lookup.

Run a subset with `python .mutate_re.py R1`.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_RES_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptResources\ScriptResources.cpp")
F_FS_H = os.path.join(ROOT, r"engine\Source\Runtime\File\FileSystem.h")
EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    text = io.open(path, encoding="utf-8").read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def build_and_run():
    p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0 and ("LNK1236" in p.stdout or "LNK1000" in p.stdout or "LNK1314" in p.stdout):
        subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                           cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-6:]:
            print("    " + line)
        return "build-failed"
    r = subprocess.run([os.path.join(ROOT, EXE)], cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l]
    tail = [l for l in r.stdout.splitlines() if "checks," in l]
    print("  rc=%d  %s" % (r.returncode, tail[-1] if tail else "(crashed -- no summary)"))
    for l in fails[:8]:
        print("    " + l.strip())
    if r.returncode == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("R1 Exists claims everything exists", F_RES_CPP,
     "    return !ResolveAssetPath(path).empty();",
     "    return true;"),

    ("R2 extension probing is dropped (Unity writes paths without one)", F_RES_CPP,
     "        for (const char* extension : kProbExtensions)\n"
     "        {\n"
     "            std::filesystem::path with_extension = base;\n"
     "            with_extension += extension;\n"
     "            if (std::filesystem::exists(with_extension, ec))\n"
     "            {\n"
     "                return std::filesystem::absolute(with_extension).lexically_normal();\n"
     "            }\n"
     "        }\n",
     ""),

    ("R3 the absolute-path fast path is dropped", F_RES_CPP,
     "        if (relative.is_absolute())\n        {\n            return TryCandidate(relative);\n        }\n",
     ""),

    ("R4 GetResolvedPath echoes the caller's spelling", F_RES_CPP,
     "    return resolved.empty() ? eastl::string() : eastl::string(resolved.generic_string().c_str());",
     "    return eastl::string(path.c_str());"),

    ("R5 GetTypeName never reports a type", F_RES_CPP,
     "    return (asset != nullptr) ? eastl::string(asset->GetTypeName()) : eastl::string();",
     "    return eastl::string();"),

    ("R6 Load never hands the object back", F_RES_CPP,
     "    cache.emplace(key, object);\n    return object;",
     "    cache.emplace(key, object);\n    return nullptr;"),

    ("R7 Load never fills the cache", F_RES_CPP,
     "    cache.emplace(key, object);\n    return object;",
     "    (void)key;\n    return object;"),

    ("R8 UnloadAsset leaves the entry in place", F_RES_CPP,
     "        if (it->second == asset)\n        {\n            cache.erase(it);\n            return true;\n        }",
     "        if (it->second == asset)\n        {\n            return true;\n        }"),

    ("R9 UnloadUnusedAssets keeps the cache", F_RES_CPP,
     "    const int dropped = static_cast<int>(cache.size());\n    cache.clear();\n    return dropped;",
     "    const int dropped = static_cast<int>(cache.size());\n    return dropped;"),

    ("R10 LoadTyped falls back to an untyped load for an unknown type", F_RES_CPP,
     "        LOG_ERROR(ZAsset, \"Resources.LoadTyped('{}', '{}'): no such class is registered\", path.c_str(), type_name.c_str());\n"
     "        return nullptr;",
     "        LOG_ERROR(ZAsset, \"Resources.LoadTyped('{}', '{}'): no such class is registered\", path.c_str(), type_name.c_str());\n"
     "        return Load(path);"),

    ("R11 the Unity Resources/ folder convention is dropped", F_RES_CPP,
     "            for (const char* prefix : {\"Resources\", \"\"})",
     "            for (const char* prefix : {\"\"})"),

    ("R12 loadAssetJson hands back null instead of the object", F_FS_H,
     "            JSONUtility::DeserializedFromJSONRead(reader, *obj);\n"
     "            reader.ResetCurrentNodeToOriginalState();\n"
     "            return obj;",
     "            JSONUtility::DeserializedFromJSONRead(reader, *obj);\n"
     "            reader.ResetCurrentNodeToOriginalState();\n"
     "            return nullptr;"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".rebak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
            os.utime(path, None)
            touched.add(path)
        print()
    for path in touched:
        os.utime(path, None)
    print("---- summary ----")
    for n, r in results:
        print("%-62s %s" % (n, r))


if __name__ == "__main__":
    main()
