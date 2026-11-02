"""Mutation harness for the Material / Light slice (#52).

What is under test
------------------
`renderer.material` is a *handle* (a `MaterialInstance` bound to
{renderer, slot}) over the per-sub-mesh runtime material override, and
`Light` grew Unity's `color` / `intensity` / `range` / `spotAngle` face over
the engine's physical `flux` (per-channel radiant flux) and cone half-angles
in radians.

Why these particular mutants
----------------------------
* **A handle that is not cached is still correct but allocates per frame.**
  ML01 catches the cache because the *identity* of `renderer.material` is the
  only observable difference -- the values live on the renderer, so a
  "return a fresh handle each call" implementation passes every value
  assertion. That is exactly the kind of change someone makes while
  "simplifying" and never notices.
* **Slot isolation is the entire point of a per-sub-mesh override.** Tinting
  one sub-mesh and tinting all of them is invisible to a single-slot test.
  ML02/ML03 aim at the slot plumbing.
* **The unknown-name sentinel must come from Has*, not from the value.** A
  colour property that exists and is genuinely black must read as black, so
  "return white when the value is zero" is wrong even though it passes every
  test that only uses non-black colours. ML12 is that mutant.
* **HasProperty is a union of three lookups.** Dropping the vector or the
  texture arm makes `HasProperty` lie about half the property set, and the
  lie only shows up on the kind of property the remaining arm does not cover
  (ML08/ML09, caught by MI37/MI38).
* **The flux decomposition must be reversible.** `intensity = max(flux)`,
  `color = flux / intensity` is exact; using a per-channel split or a sum
  drifts the hue in a way that looks like "the colour space is wrong"
  (LL01..LL04).
* **spotAngle is a full cone angle in degrees; the engine stores a half cone
  angle in radians.** Getting one of the two factors wrong still looks like a
  spotlight -- it is just the wrong size (LL06..LL09).

Run:  py .mutate_ml.py            (all)
      py .mutate_ml.py ML01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


MAT_H = S(r"engine/Source/Runtime/Scripting/ScriptMaterial/ScriptMaterial.h")
REN_H = S(r"engine/Source/Runtime/Function/Framework/Component/Mesh/BaseRenderer.h")
REN_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Mesh/BaseRenderer.cpp")
LIGHT_H = S(r"engine/Source/Runtime/Function/Framework/Component/Light/LightComponent.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# .cpp 里的目标函数在文件作用域，函数收尾是顶格的 `}`。
END_OF_FUNC = "\n}\n"
# ⚠️ ScriptMaterial.h / LightComponent.h 里的目标函数在**类**里：函数体 8 空格，
# 函数收尾是 4 空格的 `}`。用顶格标记会一路跑到类外面，把无关代码也改掉。
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else marker_for(path)
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r" % func_sig)
    region = text[i:j]
    if old not in region:
        raise SystemExit("PATTERN NOT FOUND in %s [%s]:\n%r" % (path, func_sig, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        text[:i] + region.replace(old, new, 1) + text[j:])


def run(exe):
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout or ""
    fails = [l for l in out.splitlines() if "FAIL" in l]
    tail = [l for l in out.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-30s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    last = None
    for _ in range(3):
        ok = True
        for target in TARGETS:
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                               env=ENV, capture_output=True, text=True, timeout=1800)
            last = p
            if p.returncode != 0:
                ok = False
                # LNK1236 是 ZRuntime.lib 的 COFF 段被写坏了（MSBuild 并行撞车），
                # 不是代码错。单编一次 ZRuntime 再继续。
                if "LNK1236" in (p.stdout or ""):
                    subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime",
                                    "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                                   env=ENV, capture_output=True, text=True, timeout=1800)
                break
        if ok:
            return last
    return last


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # =========================================================================
    # renderer.material：句柄的身份、槽位与越界
    # =========================================================================
    # ⚠️ 值断言抓不到这条：值本来就在 renderer 上，句柄是新的也照样读得对。
    # 唯一可观测的差别是**身份**，所以 MI5 那条"两次拿到同一个对象"是它的对手。
    ("ML01 GetMaterial mints a new handle every call (the cache is gone)",
     REN_CPP, "MaterialInstance* BaseRenderer::GetMaterial(int slot)",
     "    MaterialInstance* handle = m_MaterialHandles[slot];",
     "    MaterialInstance* handle = nullptr;"),

    ("ML02 GetMaterial ignores the slot range",
     REN_CPP, "MaterialInstance* BaseRenderer::GetMaterial(int slot)",
     "    if (slot < 0 || slot >= static_cast<int>(m_SubMeshes.size()))\n"
     "    {\n        return nullptr;\n    }",
     "    if (false)\n    {\n        return nullptr;\n    }"),

    ("ML03 GetMainMaterial hands out the last slot instead of slot 0",
     REN_H, None,
     "MaterialInstance* GetMainMaterial() { return GetMaterial(0); }",
     "MaterialInstance* GetMainMaterial() { return GetMaterial(GetMaterialCount() - 1); }"),

    ("ML04 GetMaterialCount is off by one",
     REN_H, None,
     "int GetMaterialCount() const { return static_cast<int>(m_SubMeshes.size()); }",
     "int GetMaterialCount() const { return static_cast<int>(m_SubMeshes.size()) + 1; }"),

    ("ML05 GetMaterial binds the handle to the wrong slot",
     REN_CPP, "MaterialInstance* BaseRenderer::GetMaterial(int slot)",
     "handle->Bind(this, slot);", "handle->Bind(this, 0);"),

    # =========================================================================
    # Material 句柄：主色与具名属性
    # =========================================================================
    ("ML06 material.color drops alpha (always writes w = 1)",
     MAT_H, "void SetMainColor(const Color& color)",
     "renderer->SetBaseColor(m_Slot, Vector4(color.r, color.g, color.b, color.a));",
     "renderer->SetBaseColor(m_Slot, Vector4(color.r, color.g, color.b, 1.0f));"),

    ("ML07 an unbound handle reads back transparent black instead of white",
     MAT_H, "Color GetMainColor() const",
     ": Vector4(1.0f, 1.0f, 1.0f, 1.0f);",
     ": Vector4(0.0f, 0.0f, 0.0f, 0.0f);"),

    ("ML08 HasProperty only looks at scalar properties",
     REN_CPP, "bool BaseRenderer::HasMaterialProperty(int slot, const eastl::string& name) const",
     "return HasMaterialFloat(slot, name) || HasMaterialVector(slot, name) || "
     "HasMaterialTexture(slot, name);",
     "return HasMaterialFloat(slot, name);"),

    ("ML09 HasProperty only looks at the shader reflection table",
     REN_CPP, "bool BaseRenderer::HasMaterialProperty(int slot, const eastl::string& name) const",
     "return HasMaterialFloat(slot, name) || HasMaterialVector(slot, name) || "
     "HasMaterialTexture(slot, name);",
     "return HasMaterialTexture(slot, name);"),

    # ⚠️ 变异的是**回落分支**而不是 shader 反射表那支：这个测试里没有 shader 资产，
    # `desc->m_ShaderProperties` 是空的，`property != nullptr` 那支根本进不去 ——
    # 变异它会得到一个"在这个环境里等价"的变异体（谁也抓不到，不是测试弱）。
    ("ML10 HasFloat also accepts vector properties",
     REN_CPP, "bool BaseRenderer::HasMaterialFloat(int slot, const eastl::string& name) const",
     "    return MaterialCustomParams::FindFloat(name) != nullptr;",
     "    return MaterialCustomParams::FindFloat(name) != nullptr ||\n"
     "           MaterialCustomParams::FindVector(name) != nullptr;"),

    ("ML11 HasVector also accepts scalar properties",
     REN_CPP, "bool BaseRenderer::HasMaterialVector(int slot, const eastl::string& name) const",
     "    return MaterialCustomParams::FindVector(name) != nullptr;",
     "    return MaterialCustomParams::FindVector(name) != nullptr ||\n"
     "           MaterialCustomParams::FindFloat(name) != nullptr;"),

    # ⚠️ 这条是"拿返回值当哨兵"：存在且真的是黑的属性会被当成不存在。
    ("ML12 GetColor treats a zero value as 'no such property'",
     MAT_H, "Color GetColor(const eastl::string& name) const",
     "const bool have = (m_Renderer != nullptr) && m_Renderer->HasMaterialVector(m_Slot, name);",
     "const bool have = (m_Renderer != nullptr) &&\n"
     "                  m_Renderer->GetVector(m_Slot, name) != Vector4(0.0f, 0.0f, 0.0f, 0.0f);"),

    ("ML13 SetFloat reports success even for a name nobody knows",
     MAT_H, "bool SetFloat(const eastl::string& name, float value)",
     "return (m_Renderer != nullptr) && m_Renderer->SetFloat(m_Slot, name, value);",
     "m_Renderer->SetFloat(m_Slot, name, value);\n        return m_Renderer != nullptr;"),

    ("ML14 EnableKeyword is a no-op",
     MAT_H, "void EnableKeyword(const eastl::string& keyword)",
     "            m_Renderer->EnableKeyword(m_Slot, keyword);", "            ;"),

    ("ML15 IsKeywordEnabled always reports enabled",
     MAT_H, "bool IsKeywordEnabled(const eastl::string& keyword) const",
     "return (m_Renderer != nullptr) && m_Renderer->HasKeyword(m_Slot, keyword);",
     "return m_Renderer != nullptr;"),

    ("ML16 GetTexture reports the asset path even when the slot was overridden",
     MAT_H, "eastl::string GetTexture(const eastl::string& name) const",
     "return (m_Renderer != nullptr) ? m_Renderer->GetTexture(m_Slot, name) : eastl::string();",
     'return (m_Renderer != nullptr) ? eastl::string("") : eastl::string();'),

    ("ML17 an unbound handle claims to be valid",
     MAT_H, "bool IsValid() const",
     "return m_Renderer != nullptr;", "return true;"),

    # =========================================================================
    # Light：color × intensity <-> flux
    # =========================================================================
    ("LL01 intensity is the sum of the channels instead of the max",
     LIGHT_H, None,
     "return std::max({m_Flux.x, m_Flux.y, m_Flux.z, 0.0f});",
     "return m_Flux.x + m_Flux.y + m_Flux.z;"),

    ("LL02 GetLightColor normalises per channel (the hue drifts)",
     LIGHT_H, "Color GetLightColor() const",
     "            result.r = m_Flux.x / intensity;\n"
     "            result.g = m_Flux.y / intensity;\n"
     "            result.b = m_Flux.z / intensity;",
     "            result.r = 1.0f;\n"
     "            result.g = m_Flux.y / m_Flux.x;\n"
     "            result.b = m_Flux.z / m_Flux.x;"),

    ("LL03 SetLightColor forgets the intensity (writes the colour as the flux)",
     LIGHT_H, "void SetLightColor(const Color& color)",
     "        const float intensity = GetIntensity();\n"
     "        m_Flux = Vector3(color.r * intensity, color.g * intensity, color.b * intensity);",
     "        m_Flux = Vector3(color.r, color.g, color.b);"),

    ("LL04 SetIntensity forgets the hue (writes a flat flux)",
     LIGHT_H, "void SetIntensity(float intensity)",
     "        const Color color = GetLightColor();\n"
     "        m_Flux = Vector3(color.r * intensity, color.g * intensity, color.b * intensity);",
     "        m_Flux = Vector3(intensity, intensity, intensity);"),

    ("LL05 a zero-flux light reports black instead of white",
     LIGHT_H, "Color GetLightColor() const",
     "            result.r = 1.0f;\n            result.g = 1.0f;\n            result.b = 1.0f;",
     "            result.r = 0.0f;\n            result.g = 0.0f;\n            result.b = 0.0f;"),

    ("LL06 GetSpotAngle forgets the factor of 2 (reports the half angle)",
     LIGHT_H, None,
     "return 2.0f * m_OuterConeAngle * Math_fRad2Deg;",
     "return m_OuterConeAngle * Math_fRad2Deg;"),

    ("LL07 SetSpotAngle forgets the factor of 0.5 (stores the full angle)",
     LIGHT_H, None,
     "m_OuterConeAngle = (degrees * 0.5f) / Math_fRad2Deg;",
     "m_OuterConeAngle = degrees / Math_fRad2Deg;"),

    ("LL08 SetSpotAngle converts the wrong way (multiplies by Rad2Deg)",
     LIGHT_H, None,
     "m_OuterConeAngle = (degrees * 0.5f) / Math_fRad2Deg;",
     "m_OuterConeAngle = (degrees * 0.5f) * Math_fRad2Deg;"),

    ("LL09 SetInnerSpotAngle writes the outer cone",
     LIGHT_H, None,
     "m_InnerConeAngle = (degrees * 0.5f) / Math_fRad2Deg;",
     "m_OuterConeAngle = (degrees * 0.5f) / Math_fRad2Deg;"),

    ("LL10 SetRange is a no-op",
     LIGHT_H, None,
     "void SetRange(float range) { m_Radius = range; }",
     "void SetRange(float range) { (void)range; }"),

    ("LL11 color.a is silently kept instead of being ignored",
     LIGHT_H, "Color GetLightColor() const",
     "        result.a = 1.0f;", "        result.a = 0.0f;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_ml.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_ml.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            end_marker = None
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            elif len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".mlbak")
            shutil.copy2(path, bak)
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new, end_marker)
                    else:
                        mutate(path, old, new)
                except SystemExit as e:
                    print("  HARNESS ERROR: %s" % e)
                    results.append((name, "harness error (anchor not found)"))
                    continue
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
                with io.open(os.path.join(ROOT, ".mutate_ml.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
