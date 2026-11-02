"""Mutation harness for the object lifecycle slice (AM/AN checks)."""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_GO_H = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.h")
F_GO_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
F_LV_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Level\Level.h")
F_LV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Level\Level.cpp")
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
    ("M1 the destroy timers never count down", F_LV_CPP,
     "    for (auto& entry : m_DelayedDestroys)\n"
     "    {\n"
     "        entry.second -= delta_time;\n"
     "    }",
     "    for (auto& entry : m_DelayedDestroys)\n"
     "    {\n"
     "        (void)entry;\n"
     "    }"),

    ("M2 a delayed destroy fires on queue instead", F_LV_CPP,
     "    m_DelayedDestroys.emplace_back(go_id, delay_seconds);",
     "    QueueDestroy(go_id);"),

    ("M3 Destroy(obj, 0) goes through the timer anyway", F_LV_CPP,
     "    if (delay_seconds <= 0.f)\n"
     "    {\n"
     "        QueueDestroy(go_id);\n"
     "        return;\n"
     "    }",
     "    if (false)\n"
     "    {\n"
     "        QueueDestroy(go_id);\n"
     "        return;\n"
     "    }"),

    ("M4 a later request pushes the deadline out", F_LV_CPP,
     "            if (delay_seconds < entry.second)\n"
     "            {\n"
     "                entry.second = delay_seconds;\n"
     "            }\n"
     "            return;",
     "            entry.second = delay_seconds;\n"
     "            return;"),

    ("M5 Level::Tick does not drive the timers", F_LV_CPP,
     "    TickDestroyTimers(delta_time);\n",
     ""),

    ("M6 the type search skips runtime objects", F_GO_CPP,
     "    for (const auto& id_object_pair : level->GetRuntimeGObjects())\n"
     "    {\n"
     "        collect_from(id_object_pair.second);\n"
     "    }\n"
     "    return found;",
     "    return found;"),

    ("M7 the type search keeps only the first match per object", F_GO_CPP,
     "        for (Component* component : object->GetComponentsByTypeName(component_type_name))\n"
     "        {\n"
     "            found.push_back(component);\n"
     "        }",
     "        if (Component* component = object->GetComponentByTypeName(component_type_name))\n"
     "        {\n"
     "            found.push_back(component);\n"
     "        }"),

    ("M8 DestroyIn drops the delay on the floor", F_GO_CPP,
     "        level->QueueDestroy(gobject->GetID(), delay);\n"
     "    }",
     "        level->QueueDestroy(gobject->GetID());\n"
     "    }"),

    ("M9 DestroyIn never flushes (DestroyImmediate is not immediate)", F_GO_CPP,
     "    if (flush_now)\n"
     "    {\n"
     "        level->FlushPendingDestroys();\n"
     "    }",
     "    if (false)\n"
     "    {\n"
     "        level->FlushPendingDestroys();\n"
     "    }"),

    ("M10 FindObjectsOfType is silently not exported", F_GO_H,
     "    ZFUNCTION(Name=\"FindObjectsOfType\")\n"
     "    static std::vector<Component*> FindAllComponentsOfType",
     "    static std::vector<Component*> FindAllComponentsOfType"),

    # ⚠️ 只能拿 DestroyIn 的判空做这个变异，不能拿 DestroyObject 的：壳在
    # headless 下先撞上"没有活动关卡"那条 return，根本到不了判空 —— 摘掉它
    # 测试全绿（M11 第一次就是这么漏过去的）。这也是为什么销毁逻辑要拆出
    # 一个吃明确 Level 的核心。
    ("M11 DestroyIn loses its null-object guard", F_GO_CPP,
     "void GameObject::DestroyIn(Level* level, GameObject* gobject, float delay, bool flush_now)\n"
     "{\n"
     "    if (gobject == nullptr)\n"
     "    {\n"
     "        return;\n"
     "    }",
     "void GameObject::DestroyIn(Level* level, GameObject* gobject, float delay, bool flush_now)\n"
     "{\n"
     "    if (false)\n"
     "    {\n"
     "        return;\n"
     "    }"),

    ("M12 DestroyIn loses its null-level guard", F_GO_CPP,
     "    if (level == nullptr)\n"
     "    {\n"
     "        return;\n"
     "    }",
     "    if (false)\n"
     "    {\n"
     "        return;\n"
     "    }"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".olbak")
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
