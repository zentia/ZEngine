"""Mutation harness for the Camera-extras slice (AR / AS checks).

Same contract as the other .mutate_*.py: every mutation must make the smoke
test go red. Restore ends with `touch` -- see .mutate_pm.py for why that
matters on this machine.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_CAM_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptCamera\ScriptCamera.h")
F_CAM_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptCamera\ScriptCamera.cpp")
F_RC_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Render\RenderCamera.h")
F_RC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Render\RenderCamera.cpp")
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
    if p.returncode != 0 and ("LNK1236" in p.stdout or "LNK1000" in p.stdout):
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
    ("M1 ScreenToViewportPoint divides the depth too", F_CAM_CPP,
     "    return Vector3(screen_point.x / width, screen_point.y / height, screen_point.z);",
     "    return Vector3(screen_point.x / width, screen_point.y / height, screen_point.z / height);"),

    ("M2 ScreenToViewportPoint swaps width and height", F_CAM_CPP,
     "    return Vector3(screen_point.x / width, screen_point.y / height, screen_point.z);",
     "    return Vector3(screen_point.x / height, screen_point.y / width, screen_point.z);"),

    ("M3 ViewportToScreenPoint swaps width and height", F_CAM_CPP,
     "    return Vector3(viewport_point.x * width, viewport_point.y * height, viewport_point.z);",
     "    return Vector3(viewport_point.x * height, viewport_point.y * width, viewport_point.z);"),

    ("M4 cameraToWorldMatrix is the view matrix, not its inverse", F_CAM_CPP,
     "    return m_RenderCamera->GetViewMatrix().inverse();",
     "    return m_RenderCamera->GetViewMatrix();"),

    ("M5 cameraToWorldMatrix is the identity", F_CAM_CPP,
     "    return m_RenderCamera->GetViewMatrix().inverse();",
     "    return Matrix4x4::IDENTITY;"),

    ("M6 GetNonJitteredProjectionMatrix stops removing the jitter", F_CAM_CPP,
     "    return (m_RenderCamera != nullptr) ? m_RenderCamera->GetProjectionMatrixNoJitter() : Matrix4x4::IDENTITY;",
     "    return (m_RenderCamera != nullptr) ? m_RenderCamera->GetProjectionMatrix() : Matrix4x4::IDENTITY;"),

    ("M7 ResetAspect becomes a no-op", F_CAM_CPP,
     "        m_RenderCamera->ResetAspect();",
     "        (void)0;"),

    ("M8 GetPixelRect reports no size", F_CAM_CPP,
     "    return Vector4(rect.x, rect.y, rect.width, rect.height);",
     "    return Vector4(rect.x, rect.y, 0.0f, 0.0f);"),

    ("M9 GetForward returns up", F_CAM_CPP,
     "    return (m_RenderCamera != nullptr) ? m_RenderCamera->forward() : Vector3::UNIT_Y;",
     "    return (m_RenderCamera != nullptr) ? m_RenderCamera->up() : Vector3::UNIT_Y;"),

    # --- the two RenderCamera fixes the new API depends on -------------------

    ("M10 the camera rect goes back to the all-zero default", F_RC_H,
     "        Rectf normalizedViewPortRect {0.f, 0.f, 1.f, 1.f};",
     "        Rectf normalizedViewPortRect;"),

    ("M11 ResetAspect writes m_State.aspect again (the old no-op)", F_RC_CPP,
     "    SetAspect((r.height > 0.f) ? (r.width / r.height) : 1.0f);",
     "    m_State.aspect = (r.height > 0.f) ? (r.width / r.height) : 1.0f;"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".cmbak")
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
        print("%-58s %s" % (n, r))


if __name__ == "__main__":
    main()
