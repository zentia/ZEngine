"""Mutation harness for the PlayerPrefs slice (AE/AF checks)."""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptPlayerPrefs\ScriptPlayerPrefs.cpp")
F_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptPlayerPrefs\ScriptPlayerPrefs.h")
U_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Resource\UserPreferences\UserPreferences.cpp")
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
    if p.returncode != 0 and "LNK1236" in p.stdout:
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
    ("M1 GetInt ignores the default value", F_CPP,
     "        return prefs->GetInt(key.c_str(), default_value);",
     "        return prefs->GetInt(key.c_str(), 0);"),
    ("M2 SetInt is a no-op", F_CPP,
     "        prefs->SetInt(key.c_str(), value);",
     "        (void)value;"),
    ("M3 SetString is a no-op", F_CPP,
     "        prefs->SetString(key.c_str(), value.c_str());",
     "        (void)value;"),
    ("M4 HasKey guesses true with no system", F_CPP,
     '    // 没有偏好系统时"没有任何键"是唯一诚实的答案 —— 说 true 会让脚本以为读得到。\n    return false;',
     '    return true;'),
    ("M5 SetInt derefs the system without a guard", F_CPP,
     "    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)\n    {\n        prefs->SetInt(key.c_str(), value);\n    }",
     "    ResolvePreferences()->SetInt(key.c_str(), value);"),
    ("M6 DeleteAll leaves the other keys", U_CPP,
     "    m_Preferences.SetObject();\n    // SetObject",
     "    // mutated: no SetObject\n    // SetObject"),
    ("M7 save() drops the empty-path guard", U_CPP,
     "    if (m_PreferencesFilePath.empty())\n    {\n        return;\n    }",
     "    (void)0;"),
    ("M8 GetBool ignores the default", F_CPP,
     "        return prefs->GetBool(key.c_str(), default_value);",
     "        return prefs->GetBool(key.c_str(), false);"),
    ("M9 a method is silently not exported", F_H,
     "    ZFUNCTION()\n    static void DeleteAll();",
     "    static void DeleteAll();"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".ppbak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
        print()
    print("---- summary ----")
    for n, r in results:
        print("%-52s %s" % (n, r))


if __name__ == "__main__":
    main()
