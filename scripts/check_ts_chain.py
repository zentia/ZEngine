#!/usr/bin/env python3
"""TS -> tsc -> .js -> ScriptEnv -> C++ 全链路校验（需要 node）。

为什么分成两个东西
------------------
ScriptTsChainSmokeTest 只负责"tsc 的产物跑得起来、值对"，它**不需要 node** ——
因为 TscOutput/ 下的 .js 是签进仓库的。

但"签进去的产物"有个天然的失效方式：有人改了 Mover.ts / tsconfig，或者引擎的
d.ts 变了，却忘了重新编译，于是测试喂给 ScriptEnv 的其实是一份过期的 .js，
而所有断言照样绿。这个脚本堵的就是这个洞：

    1. 用 ScriptTsChainSmokeTest --dump-dts 拿**当前**的 zengine.d.ts
       （编辑器之外唯一能复现同一份内容的入口）
    2. 用 tsc 按仓库里那份 tsconfig.json 编译 Scripts/*.ts 到一个临时 outDir
    3. 与签入的 TscOutput/ 逐字节 diff —— 漂移就报错（--update 可刷新）
    4. 再跑一遍 ScriptTsChainSmokeTest，但 --js-root 指向**刚编译出来的**临时
       目录，证明新产物也跑得通
    5. 再跑一遍 ScriptComponentSmokeTest：同一批产物挂到真的 GameObject 上，
       走 PostLoadResource -> OnAwake/OnStart -> Tick -> 热重载 -> OnDestroy
    6. 最后跑 ScriptComponentPrefabTest：把同一个组件存成 .prefab（YAML）和
       .zasset（二进制 SerializedFile），再读回来验"从资产文件里还原出来的
       guid 能绑上、Inspector override 能跟着生效"

用法
----
    python3 scripts/check_ts_chain.py                 # 校验（漂移就非零退出）
    python3 scripts/check_ts_chain.py --update        # 刷新 TscOutput/
    python3 scripts/check_ts_chain.py --strict-dts    # 额外对 d.ts 本身做一次
                                                      # 不开 skipLibCheck 的检查
                                                      #（只报 warning，见下）

tsc 的发现顺序：环境变量 ZENGINE_TSC -> PATH 上的 tsc -> 装到 /tmp/ztsc。

关于 --strict-dts
-----------------
测试用的可执行文件只把 6 个类的 .gen.cpp 编了进来，所以生成的 d.ts 里没有
GameObject / Transform 这类只被 Behaviour 声明引用到的类型；对 d.ts 单独做
一次不开 skipLibCheck 的检查必然报 "Cannot find name 'GameObject'"。这是**测试
构建刻意裁剪**的结果，不是 d.ts 的 bug，所以只当 warning 打印。真实工程里
ScriptEnv 面对的是 ZRuntime 的完整注册表。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TEST_DIR = REPO_ROOT / "engine" / "Source" / "Runtime" / "Scripting" / "ScriptVM" / "Test"
TSCONFIG = TEST_DIR / "tsconfig.json"
TSCOUT = TEST_DIR / "TscOutput"
TYPINGS = TEST_DIR / "Typings"
RUN_SMOKE = REPO_ROOT / "scripts" / "run_smoke_test.py"
TS_CHAIN_SRC = TEST_DIR / "ScriptTsChainSmokeTest.cpp"

# 本机 `bin/libZRuntime.a` 比最近几次提交都旧，凡是被那些提交改过 / 新加的引擎
# 源都得显式编进来（否则 Apple 上靠 -undefined dynamic_lookup 能链过、运行期一跳
# 进去就 SIGSEGV）。CMake 构建不需要这些 —— ZRuntime 里就是最新的。
EXTRA_SOURCES = [
    "engine/Source/Runtime/Scripting/Backends/QuickJS/BackendQuickJS.cpp",
    "engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp",
    "engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp",
    "build/Generated/ZHT/ZRuntime/Src/ScriptMath.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/ScriptTime.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/ScriptInput.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/KeyCodes.gen.cpp",
    "engine/Source/Runtime/Core/Time/TimeState.cpp",
]

# TypeScriptComponent 真身在 ScriptTsChain 那套之外还要的几份：
#   * TypeManager.cpp —— .a 里那份的 TypeRegistrationDesc 布局和现编的 .gen.cpp
#     对不上，RegisterType 读到的 desc.type 是错位后的 nullptr，GameObject /
#     Transform / TypeScriptComponent 全注册不进类型表。
#   * GameObject.cpp + GameObject/Transform/Behaviour/TypeScriptComponent 的
#     .gen.cpp —— AddComponentByTypeName 和 Cast<Component> 都走类型表。
SCRIPT_COMPONENT_SRC = TEST_DIR / "ScriptComponentSmokeTest.cpp"
SCRIPT_COMPONENT_EXTRA_SOURCES = EXTRA_SOURCES + [
    "engine/Source/Runtime/BaseClasses/TypeManager.cpp",
    "engine/Source/Runtime/BaseClasses/GameObject.cpp",
    # 组件本身也要现编：否则测的是 .a 里那份，改了源码测试不会红。
    "engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp",
    "build/Generated/ZHT/ZRuntime/Src/GameObject.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/Transform.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/Behaviour.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/TypeScriptComponent.gen.cpp",
]

# prefab / zasset 反序列化那条路在 ScriptComponent 之外还要的几份：
#   * PrefabUtility.cpp + PrefabSubtreeCloner.cpp —— .a 那份里根本没有
#     LoadPrefabAsset（整图加载是 .a 构建之后才加的），链出来是
#     "dynamically looked up"，一跳进去就崩。
#   * PrefabAsset / PrefabRefComponent / RigidbodyComponent 的 .gen.cpp ——
#     PrefabUtility 里的 TypeOf<PrefabAsset> / Cast<PrefabRefComponent> /
#     Cast<RigidBodyComponent> 要它们的 TypeContainer<T>::m_Type。
#   * 序列化层的五份（见下 ARCHIVE_MEMBER_EXCLUSIONS）：这条路径是"按旧布局
#     构造 + 按新布局序列化"最容易崩的地方。
SCRIPT_PREFAB_SRC = TEST_DIR / "ScriptComponentPrefabTest.cpp"
SCRIPT_PREFAB_EXTRA_SOURCES = SCRIPT_COMPONENT_EXTRA_SOURCES + [
    "engine/Source/Runtime/Resource/Prefab/PrefabUtility.cpp",
    "engine/Source/Runtime/Resource/Prefab/PrefabSubtreeCloner.cpp",
    "engine/Source/Runtime/Core/YamlSerialize/YamlObjectGraph.cpp",
    "engine/Source/Runtime/Resource/Asset/AssetManager.cpp",
    "engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp",
    "engine/Source/Runtime/BaseClasses/IPPtrResolver.cpp",
    "engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp",
    "build/Generated/ZHT/ZRuntime/Src/PrefabAsset.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/PrefabRefComponent.gen.cpp",
    "build/Generated/ZHT/ZRuntime/Src/RigidbodyComponent.gen.cpp",
    # 2026-10-27 从 Editor/Prefab/ 下沉过来的 override 管线，.a 里没有：
    "engine/Source/Runtime/Resource/Prefab/PropertyModification.cpp",
    "engine/Source/Runtime/Resource/Prefab/PropertyPathResolver.cpp",
    "engine/Source/Runtime/Resource/Prefab/PropertyValueLocator.cpp",
]

# 上面这五份 .cpp 在 .a 里都有同名成员，且会因为别处引用了它们独有的旧符号
# 被拉进来 —— 两边都定义同一批符号就是 duplicate symbol，所以要把成员踢掉。
ARCHIVE_MEMBER_EXCLUSIONS = [
    "YamlObjectGraph.cpp.o",
    "AssetManager.cpp.o",
    "ImmediatePtr.cpp.o",
    "IPPtrResolver.cpp.o",
    "Transform.cpp.o",
]


def find_tsc() -> list[str]:
    env = os.environ.get("ZENGINE_TSC")
    if env:
        return [env]
    on_path = shutil.which("tsc")
    if on_path:
        return [on_path]
    cached = Path("/tmp/ztsc/node_modules/.bin/tsc")
    if not cached.is_file():
        print("[tsc] 本机没找到 tsc，装一份到 /tmp/ztsc ...", flush=True)
        subprocess.run(
            ["npm", "install", "--prefix", "/tmp/ztsc", "typescript@5.6.3",
             "--no-audit", "--no-fund"],
            check=True)
    return [str(cached)]


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd or REPO_ROOT), capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="TS -> tsc -> ScriptEnv 全链路校验")
    ap.add_argument("--update", action="store_true", help="刷新签入的 TscOutput/")
    ap.add_argument("--strict-dts", action="store_true", help="额外检查 d.ts 本身（只报 warning）")
    ap.add_argument("--skip-build", action="store_true", help="跳过 dump-dts 的构建步骤")
    args = ap.parse_args()

    dts_tmp = Path(tempfile.mkdtemp(prefix="zts_chain_"))
    dts_path = dts_tmp / "Typings" / "zengine.d.ts"
    out_dir = dts_tmp / "out"

    # 1) 当前的 zengine.d.ts
    if not args.skip_build:
        print("[1/6] 导出当前 zengine.d.ts ...", flush=True)
        cmd = [sys.executable, str(RUN_SMOKE), str(TS_CHAIN_SRC),
               "--template", "Function/Audio/AudioSystem.cpp"]
        for s in EXTRA_SOURCES:
            cmd += ["--extra-source", s]
        cmd += ["--run-arg=--dump-dts", f"--run-arg={dts_path}"]
        r = run(cmd)
        if "[dump-dts]" not in r.stdout:
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            print("导出 d.ts 失败")
            return 1
    else:
        dts_path.parent.mkdir(parents=True, exist_ok=True)

    # 2) tsc 编译
    print("[2/6] tsc 编译 Scripts/*.ts ...", flush=True)
    TYPINGS.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(dts_path, TYPINGS / "zengine.d.ts")
    tsc = find_tsc()
    r = run(tsc + ["-p", str(TSCONFIG), "--outDir", str(out_dir)])
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print("tsc 编译失败 —— 生成的 d.ts 没被类型检查器接受，或 Mover.ts 有错")
        return 1
    print(f"  编译通过，产物在 {out_dir}")

    if args.strict_dts:
        r2 = run(tsc + ["--noEmit", "--skipLibCheck", "false", str(dts_path)])
        if r2.returncode != 0:
            print("[warning] d.ts 单独检查（不开 skipLibCheck）有报错：")
            sys.stdout.write(r2.stdout)
            print("  这是测试构建只编了 6 个类的 .gen.cpp 造成的（缺 GameObject /")
            print("  Transform 之类只被声明引用到的类型），不是 d.ts 本身的问题。")
        else:
            print("[strict-dts] d.ts 单独检查通过")

    # 3) 与签入产物 diff
    print("[3/6] 与签入的 TscOutput/ 比对 ...", flush=True)
    fresh = sorted(p.name for p in out_dir.iterdir() if p.is_file())
    if not fresh:
        print("tsc 没产出任何文件")
        return 1
    drifted = []
    for name in fresh:
        pinned = TSCOUT / name
        src = (out_dir / name).read_bytes()
        if args.update:
            TSCOUT.mkdir(parents=True, exist_ok=True)
            pinned.write_bytes(src)
            print(f"  刷新 {name} ({len(src)} 字节)")
            continue
        if not pinned.is_file():
            drifted.append(f"{name}（签入产物里没有）")
            continue
        if pinned.read_bytes() != src:
            drifted.append(name)
    if drifted:
        print("  漂移： " + ", ".join(drifted))
        print("  用 --update 刷新（确认过 diff 再刷）。")
        return 1
    print("  一致")

    # 4) 跑测试，指向刚编译出来的产物
    print("[4/6] 跑 ScriptTsChainSmokeTest（js root = 刚编译出来的目录）...", flush=True)
    cmd = [sys.executable, str(RUN_SMOKE), str(TS_CHAIN_SRC),
           "--template", "Function/Audio/AudioSystem.cpp"]
    for s in EXTRA_SOURCES:
        cmd += ["--extra-source", s]
    cmd += ["--run-arg=--js-root", f"--run-arg={out_dir}"]
    r = run(cmd)
    # 过滤掉编译噪声，只留测试输出
    for line in r.stdout.splitlines():
        if line.startswith("[build]") or line.startswith("In file included"):
            continue
        print(line)
    if r.returncode != 0:
        return r.returncode

    # 5) 再跑一遍组件侧：同一批 tsc 产物挂到 GameObject 上跑完整生命周期。
    #    （本测试自己造临时工程，把 Scripts/ 和 TscOutput/ 拷过去，所以传的是
    #     resource-dir 而不是 js root。）
    print("[5/6] 跑 ScriptComponentSmokeTest（TypeScriptComponent 真身）...", flush=True)
    cmd = [sys.executable, str(RUN_SMOKE), str(SCRIPT_COMPONENT_SRC),
           "--template", "Function/Audio/AudioSystem.cpp"]
    for s in SCRIPT_COMPONENT_EXTRA_SOURCES:
        cmd += ["--extra-source", s]
    cmd += ["--run-arg=--resource-dir", f"--run-arg={TEST_DIR}"]
    r = run(cmd)
    for line in r.stdout.splitlines():
        if line.startswith("[build]") or line.startswith("In file included"):
            continue
        print(line)
    if r.returncode != 0:
        return r.returncode

    # 6) 最后跑 prefab / zasset 反序列化：把同一个组件先存成 .prefab（YAML）
    #    和 .zasset（二进制），再读回来走 PostLoadResource —— 验的是"从资产
    #    文件里还原出来的 guid 能不能绑上、override 能不能跟着生效"。
    print("[6/6] 跑 ScriptComponentPrefabTest（.prefab / .zasset 反序列化）...", flush=True)
    cmd = [sys.executable, str(RUN_SMOKE), str(SCRIPT_PREFAB_SRC),
           "--template", "Function/Audio/AudioSystem.cpp"]
    for s in SCRIPT_PREFAB_EXTRA_SOURCES:
        cmd += ["--extra-source", s]
    for m in ARCHIVE_MEMBER_EXCLUSIONS:
        cmd += ["--exclude-archive-member", m]
    cmd += ["--run-arg=--resource-dir", f"--run-arg={TEST_DIR}"]
    r = run(cmd)
    for line in r.stdout.splitlines():
        if line.startswith("[build]") or line.startswith("In file included"):
            continue
        print(line)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
