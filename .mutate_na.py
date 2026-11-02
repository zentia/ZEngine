"""Mutation harness for the navigation-agent slice (AT / AU checks).

Same contract as the other .mutate_*.py: every mutation must make the smoke
test go red. Restore ends with `touch` -- a fast mutate/build/restore round
trip can otherwise leave MSBuild believing a TU is up to date, and the
resulting crash has nothing to do with the mutation (see .mutate_pm.py).

Cost note
---------
Mutations in `NavMeshAgentComponent.cpp` are cheap (one TU + link).
Mutations in `ScriptNavMesh.h` are NOT: ZHeaderTool rewrites every generated
header, so all of ZRuntime recompiles (~1h). They are kept at three and are
worth it because that file holds logic no .cpp mutation can reach.
Run a subset with `python .mutate_na.py M19`.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_AGENT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\AI\Navigation\NavMeshAgentComponent.cpp")
F_AGENT_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\AI\Navigation\NavMeshAgentComponent.h")
F_FACADE_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptNavigation\ScriptNavMesh.h")
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
    # ---- cheap: agent .cpp ------------------------------------------------

    ("M1 autoBraking is dropped (arrives at full speed)", F_AGENT_CPP,
     "        desired_speed = Math::min(desired_speed, braking_speed);",
     "        desired_speed = desired_speed; (void)braking_speed;"),

    ("M2 acceleration limit removed (first frame teleports)", F_AGENT_CPP,
     "    const float max_velocity_delta = m_Acceleration * delta_time;",
     "    const float max_velocity_delta = 1.0e6f;"),

    ("M3 the speed clamp is gone", F_AGENT_CPP,
     "    if (m_Velocity.length() > m_Speed)",
     "    if (m_Velocity.length() > m_Speed * 100.f)"),

    ("M4 updatePosition is ignored (always writes the transform)", F_AGENT_CPP,
     "    if (m_UpdatePosition)\n    {\n        transform->SetPosition(new_position);\n    }",
     "    {\n        transform->SetPosition(new_position);\n    }"),

    ("M5 updateRotation is ignored (always turns)", F_AGENT_CPP,
     "    if (m_UpdateRotation && m_Velocity.squaredLength() > 1e-6f)",
     "    if (m_Velocity.squaredLength() > 1e-6f)"),

    ("M6 Stop leaves the velocity alone", F_AGENT_CPP,
     "    m_IsStopped = true;\n    m_Velocity = Vector3::ZERO;",
     "    m_IsStopped = true;"),

    ("M7 Warp keeps the stale path", F_AGENT_CPP,
     "    ResetPath();\n    m_NextPosition = new_position;",
     "    m_NextPosition = new_position;"),

    ("M8 steering aims at the destination instead of the next waypoint", F_AGENT_CPP,
     "    const Vector3 to_waypoint = m_Waypoints[m_WaypointIndex] - position;",
     "    const Vector3 to_waypoint = m_Destination - position;"),

    ("M9 arrival never clears hasPath (the walk loop never ends)", F_AGENT_CPP,
     "        m_HasPath = false;\n        m_Waypoints.clear();\n        m_WaypointIndex = 0;\n"
     "        m_Velocity = Vector3::ZERO;\n        // 目标**清掉**",
     "        m_Waypoints.clear();\n        m_WaypointIndex = 0;\n        m_Velocity = Vector3::ZERO;\n"
     "        // 目标**清掉**"),

    ("M10 remainingDistance forgets to subtract stoppingDistance", F_AGENT_CPP,
     "    const float remaining = ComputeRemainingDistance(transform->GetPosition()) - m_StoppingDistance;",
     "    const float remaining = ComputeRemainingDistance(transform->GetPosition());"),

    ("M11 PathPartial is never reported", F_AGENT_CPP,
     "        if (!navigation_system->IsStandable(target))\n        {\n            status = NavPathStatusValue::partial;\n        }",
     "        (void)0;"),

    ("M12 intermediate waypoints use the stopping distance (cuts corners)", F_AGENT_CPP,
     "        const float arrive_radius = is_last ? m_StoppingDistance : k_intermediate_waypoint_radius;",
     "        const float arrive_radius = m_StoppingDistance;"),

    ("M13 facing is built from +Y instead of -Y", F_AGENT_CPP,
     "    Vector3 current_forward = current_rotation * Vector3::NEGATIVE_UNIT_Y;",
     "    Vector3 current_forward = current_rotation * Vector3::UNIT_Y;"),

    ("M14 a failed SetDestination drops the destination", F_AGENT_CPP,
     "    const bool found = RequestPath(target, /*update_status=*/true);\n    m_RepathTimer = 0.f;",
     "    const bool found = RequestPath(target, /*update_status=*/true);\n    m_RepathTimer = 0.f;\n"
     "    if (!found)\n    {\n        m_HasDestination = false;\n    }"),

    ("M15 Move ignores the offset", F_AGENT_CPP,
     "    transform->SetPosition(transform->GetPosition() + offset);",
     "    (void)offset;"),

    ("M16 Resume does not force a repath", F_AGENT_CPP,
     "    m_RepathTimer = m_RepathInterval;",
     "    m_RepathTimer = 0.f;"),

    # ---- cheap: agent .h (no ZCLASS layout change, still one TU) ------------

    ("M17 the component stops overriding Tick (never runs)", F_AGENT_H,
     "    void Tick(float delta_time) override;",
     "    void Tick(float delta_time);"),

    # ---- expensive: facade header (ZHeaderTool -> full ZRuntime rebuild) ----

    ("M18 Raycast never reports blocked", F_FACADE_H,
     "            if (!navigation_system->IsStandable(sample))",
     "            if (false)"),

    ("M19 SamplePosition ignores maxDistance", F_FACADE_H,
     "        if (max_distance > 0.f && distance > max_distance)",
     "        if (false)"),

    ("M20 CalculatePath always reports PathComplete", F_FACADE_H,
     "            status = navigation_system->IsStandable(target)\n"
     "                         ? static_cast<int>(NavMeshPathStatus::PathComplete)\n"
     "                         : static_cast<int>(NavMeshPathStatus::PathPartial);",
     "            status = static_cast<int>(NavMeshPathStatus::PathComplete);"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".nabak")
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
