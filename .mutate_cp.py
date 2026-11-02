"""Mutation harness for the Component Unity facade slice (AG/AH checks)."""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_CP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Component.cpp")
F_CH = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Component.h")
F_GO_H = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.h")
F_GO_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
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
    # LNK1236 / LNK1000 都是这台机器上偶发的链接器毛病（增量链接写坏），
    # 重编 ZRuntime 再链一次就好。不重试的话"变异被捕获"里会混进假阳性。
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
    ("M1 GetName returns the component's own (empty) name", F_CP,
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetName() : name;",
     "    return name;"),

    ("M2 SetName does not write through to the host", F_CP,
     "    if (m_ParentObject != nullptr)\n    {\n        m_ParentObject->SetName(new_name);\n        return;\n    }\n    name = new_name;",
     "    name = new_name;"),

    ("M3 GetTag returns an empty string when hostless", F_CP,
     "                                       : eastl::string(TagLayerManager::kUntagged);",
     "                                       : eastl::string();"),

    ("M4 CompareTag disagrees with GetTag when hostless", F_CP,
     "    return (m_ParentObject != nullptr) ? m_ParentObject->CompareTagName(tag)\n"
     "                                       : (eastl::string(TagLayerManager::kUntagged) == tag);",
     "    return (m_ParentObject != nullptr) && m_ParentObject->CompareTagName(tag);"),

    ("M5 GetTransform derefs the host without a guard", F_CP,
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetTransform() : nullptr;",
     "    return m_ParentObject->GetTransform();"),

    ("M6 Component.GetComponents is really InChildren", F_CP,
     "std::vector<Component*> Component::GetComponentsByTypeName(const std::string& component_type_name) const\n"
     "{\n"
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentsByTypeName(component_type_name)",
     "std::vector<Component*> Component::GetComponentsByTypeName(const std::string& component_type_name) const\n"
     "{\n"
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentsInChildrenByTypeName(component_type_name)"),

    ("M7 Component.GetComponentInParent is really InChildren", F_CP,
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentInParentByTypeName(component_type_name)",
     "    return (m_ParentObject != nullptr) ? m_ParentObject->GetComponentInChildrenByTypeName(component_type_name)"),

    ("M8 SendMessage claims somebody always hears it", F_CP,
     "    return (m_ParentObject != nullptr) ? m_ParentObject->SendMessageByName(method_name) : 0;",
     "    return (m_ParentObject != nullptr) ? 1 : 0;"),

    ("M9 GetComponentsInParent excludes self", F_GO_CPP,
     "    std::vector<Component*> found;\n"
     "    for (Component* own : GetComponentsByTypeName(component_type_name))\n"
     "    {\n"
     "        found.push_back(own);\n"
     "    }",
     "    std::vector<Component*> found;"),

    ("M10 GetComponentsInParent stops after the first ancestor", F_GO_CPP,
     "        for (Component* hit : parent_object->GetComponentsByTypeName(component_type_name))\n"
     "        {\n"
     "            found.push_back(hit);\n"
     "        }\n"
     "    }\n"
     "    return found;",
     "        for (Component* hit : parent_object->GetComponentsByTypeName(component_type_name))\n"
     "        {\n"
     "            found.push_back(hit);\n"
     "        }\n"
     "        break;\n"
     "    }\n"
     "    return found;"),

    ("M11 the plural query matches on name equality (no base types)", F_GO_CPP,
     "        if (runtime_type != nullptr && wanted->IsBaseOf(runtime_type->GetRuntimeTypeIndex()))\n"
     "            found.push_back(candidate);",
     "        if (runtime_type != nullptr && runtime_type->GetRuntimeTypeIndex() == wanted->GetRuntimeTypeIndex())\n"
     "            found.push_back(candidate);"),

    ("M12 GetComponentsInParent is silently not exported", F_GO_H,
     "    ZFUNCTION(Name=\"GetComponentsInParent\")\n"
     "    std::vector<Component*> GetComponentsInParentByTypeName",
     "    std::vector<Component*> GetComponentsInParentByTypeName"),

    # ⚠️ 别拿 GetTransform 做这个变异：它还被 ZSCRIPTPROPERTY(transform) 引用，
    # 摘掉 ZFUNCTION 会让 ZHeaderTool 直接报错 —— 那是编译期拦截，测不到断言。
    ("M13 the Component facade is silently not exported", F_CH,
     "    ZFUNCTION(Name=\"GetComponentInParent\")\n"
     "    Component* GetComponentInParentByTypeName",
     "    Component* GetComponentInParentByTypeName"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".cpbak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
        print()
    print("---- summary ----")
    for n, r in results:
        print("%-58s %s" % (n, r))


if __name__ == "__main__":
    main()
