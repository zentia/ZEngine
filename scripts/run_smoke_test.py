#!/usr/bin/env python3
"""在没有 cmake 的机器上真正 build + run 一个冒烟测试。

为什么需要它
------------
zsyntax.py 只做 `-fsyntax-only`。也就是说在这台没有 cmake / ninja 的机器上，
之前所有改动的最强验证只是"编译得过"，**没有任何一行引擎代码被真正执行过**。
脚本绑定层累计到 70 个类 / 265 个方法时这一点变得很刺眼。

这个脚本补上最后一环：复用 `build/compile_commands.json` 里的一条编译命令拿
到全部 flags，把测试编成可执行文件，并**链到已有的 `bin/libZRuntime.a`**，然后
真的跑它、把退出码透出来。

关键取舍：只编译测试自己的 .cpp，引擎侧的实现从 `libZRuntime.a` 拉。
试过把 Vector3.cpp / Quaternion.cpp 一起编进来，结果 Transfer() 把序列化、
日志、mimalloc、EASTL allocator 一路拖成一片 undefined symbols —— 越补越多。
.a 是上一次 cmake 构建的产物（会过期），但它提供的正是这些稳定的底层实现；
被测的新代码是本脚本现编的，所以"过期"不影响验证新逻辑。

用法
----
    python3 scripts/run_smoke_test.py \
        engine/Source/Runtime/Scripting/ScriptMath/Test/ScriptMathSmokeTest.cpp

    # 换一条编译命令当模板（默认取 Function/Audio/AudioSystem.cpp）
    python3 scripts/run_smoke_test.py <test.cpp> --template Function/Render/RenderScene.cpp

退出码 = 测试的退出码（0 = 全部检查通过）。

本机各冒烟测试需要的 --extra-source
----------------------------------
`bin/libZRuntime.a` 比最近几次提交都旧（2026-10-26），凡是被那几次提交**改过**
的引擎源文件都得显式列进来，否则链接期看不出来（Apple 上有
`-Wl,-undefined,dynamic_lookup`），运行期一跳进去就是 SIGSEGV：

    ScriptTime    --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp
    ScriptInput   --extra-source engine/Source/Runtime/Function/Input/EnhancedInputSystem.cpp
                  --extra-source engine/Source/Runtime/Function/Input/InputSystem.cpp
    ScriptVM      见 doc/SCRIPT_VM_E2E.md 第 4 节（还要额外 4 个 .gen.cpp）
    ScriptTsChain ScriptVM 的那 6 个，再加
                  --extra-source engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp
    ScriptComponent
                  见 doc/TYPESCRIPT_COMPONENT.md 第 5 节。要点是必须带上
                  --extra-source engine/Source/Runtime/BaseClasses/TypeManager.cpp：
                  .a 里那份的 TypeRegistrationDesc 布局是旧的，现编 .gen.cpp 的
                  注册器喂进去 desc.type 会是 nullptr，类型表就是空的。
    ScriptComponentPrefab
                  ScriptComponent 那套，再加 7 份现编 + 5 个 --exclude-archive-member。
                  穿过序列化层的路径对"构造方和生成式 Transfer 是不是同一份布局"
                  特别敏感：Transform 按旧布局 new 出来、又按新布局序列化，读回来
                  就崩在 CachedReader 里。见测试文件头的"关于本机怎么跑"一节。

  一般不用手写：`scripts/check_ts_chain.py` 自带完整清单，一条命令跑完
  "导出 d.ts -> tsc 编译 -> 与签入产物 diff -> 跑测试" 四步。

症状很好认：BqLog 打完 "log_manager is constructed" 就没了，`echo $?` 是 139。
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CDB = REPO_ROOT / "build" / "compile_commands.json"
BIN_DIR = REPO_ROOT / "bin"

# 链接时按需要提供的第三方静态库。只列存在的，缺了就让链接器自己报。
LINK_LIBS = [
    "ZRuntime",
    "mimalloc",
    "spirv-cross-core",
    "spirv-cross-glsl",
    # 拉到 ShaderLab / Metal RHI 的测试还会要 msl 后端的 vtable
    # （`__ZTVN11spirv_cross11CompilerMSLE`）。它不是"要编译 shader"，只是
    # 静态初始化 / typeid 顺藤摸瓜带出来的引用，缺了就是 dyld 起步失败。
    "spirv-cross-msl",
    "astcenc",
    "bc7enc",
    "lmdb",
    # 拖到脚本层 / 资源层时才会被需要的几个：pesapi 的实现、zlib、puerts
    # 核心。libZRuntime.a 里的 ScriptEnv.cpp.o 引用 pesapi_*，
    # AssetBundleFormat.cpp.o 引用 uncompress，少了它们就是一大片
    # undefined symbols。只列存在的，不影响链不到的测试。
    "PapiQuickjs",
    "PuertsCore",
    "z",
    # 再往下（typeid(WindowSystem) 顺藤摸瓜拉进来的物理 / 资源层）：
    "Jolt",
    "minizip-ng",
    "xxhash",
    "ufbx",
    "tinyobjloader",
    "curl",
    "ZSlate",
    "ShaderLab",
]


def prepare_pruned_lib_dir(members: list[str]) -> Path:
    """造一个 bin/ 的替身目录：其余 .a 软链过去，libZRuntime.a 复制后 ar -d 掉 members。

    为什么要复制而不是改 bin/ 里那份：那是 cmake 的构建产物，改了下次 cmake
    就对不上了。按成员列表做缓存键，所以同一批排除只付一次复制的代价
    （libZRuntime.a 有 600+ MB，复制一次十几秒）。
    """
    import hashlib

    # 缓存键必须带上归档自身的指纹：libZRuntime.a 是 cmake 的构建产物，重编之后
    # 内容变了但"排除列表"没变。只按 members 做键会一直复用重编前那份副本，
    # 于是"我把 .a 刷新了"看起来毫无效果（2026-10-29：插桩进了归档却没进二进制，
    # 就是这个缓存）。
    archive = BIN_DIR / "libZRuntime.a"
    st = archive.stat()
    fingerprint = ("\n".join(sorted(members))
                   + f"\narchive:{st.st_mtime_ns}:{st.st_size}")
    key = hashlib.sha1(fingerprint.encode()).hexdigest()[:12]
    out = Path(tempfile.gettempdir()) / f"zengine_libs_{key}"
    marker = out / ".pruned"
    if marker.is_file() and marker.read_text() == fingerprint:
        return out

    out.mkdir(parents=True, exist_ok=True)
    for src in BIN_DIR.glob("*"):
        dst = out / src.name
        if dst.exists() or dst.is_symlink():
            dst.unlink()
        if src.name == "libZRuntime.a":
            shutil.copy2(src, dst)
        else:
            dst.symlink_to(src)

    subprocess.run(["ar", "-d", str(out / "libZRuntime.a")] + members,
                   check=True, capture_output=True, text=True)
    marker.write_text(fingerprint)
    return out


def build_command(template_fragment: str) -> list[str]:
    if not CDB.is_file():
        raise SystemExit(f"找不到 {CDB}：这台机器没跑过 cmake，拿不到编译 flags。")

    db = json.loads(CDB.read_text(encoding="utf-8"))
    matches = [e for e in db if template_fragment in e["file"]]
    if not matches:
        raise SystemExit(f"compile_commands.json 里没有匹配 '{template_fragment}' 的条目。")
    args = shlex.split(matches[0]["command"])

    out: list[str] = []
    skip_next = False
    i = 0
    while i < len(args):
        a = args[i]
        # 整段丢掉 PCH：`-Xclang -include-pch -Xclang <path>`。逐个 token 过滤会
        # 把后面的 -Xclang 当成"PCH 路径占位符"替换掉，clang 于是报
        # "cannot specify -o when generating multiple output files"。
        if a == "-Xclang" and i + 3 < len(args) and args[i + 1] == "-include-pch" and args[i + 2] == "-Xclang":
            i += 4
            continue
        i += 1
        if skip_next:
            skip_next = False
            continue
        if a == "-o":
            skip_next = True
            continue
        if a == "-c":
            # compile_commands 里 -c 是"编译不链接"，配 -o 会冲突。
            continue
        if a == "-fsyntax-only":
            continue
        if a.endswith(".cpp") or a.endswith(".cc") or a.endswith(".mm") or a.endswith(".pch"):
            continue
        out.append(a)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="无 cmake 也能 build + run 冒烟测试")
    parser.add_argument("test_source", help="冒烟测试的 .cpp（相对仓库根或绝对路径）")
    parser.add_argument("--template", default="Function/Audio/AudioSystem.cpp",
                        help="从 compile_commands.json 里挑哪一条命令当 flags 模板")
    parser.add_argument("--run-arg", action="append", default=[], metavar="ARG",
                        help="跑可执行文件时追加的命令行参数，可重复。用于带 argv 模式的"
                             "测试（例如 ScriptTsChainSmokeTest 的 --dump-dts / --js-root）。")
    parser.add_argument("--extra-source", action="append", default=[], metavar="CPP",
                        help="除测试自身外还要一起编译的 .cpp，可重复。用于**新加的引擎源文件**："
                             "bin/libZRuntime.a 是上一次 cmake 构建的产物，不含它们的符号，"
                             "不一起编就是 undefined symbol。")
    parser.add_argument("--exclude-archive-member", action="append", default=[], metavar="OBJ",
                        help="把 libZRuntime.a 里的这个成员（形如 Transform.cpp.o）从本次链接里"
                             "踢掉，可重复。用于 --extra-source 现编了同名 .cpp 的情况：两个"
                             "目标文件都定义同一批符号就是 duplicate symbol，而该成员仍会因为"
                             "别处引用了它独有的旧符号被拉进来。实现是复制一份 .a 到 /tmp 再 "
                             "ar -d（按成员列表缓存，只做一次）。")
    parser.add_argument("--link-map", metavar="PATH", default=None,
                        help="让链接器写一份 map 文件（-Wl,-map）。里面的 '[ N] path/lib.a(Member.o)' 列表就是"
                             "**这个测试真正链进来的归档成员**——判断『哪些过期成员需要重编』时看它，"
                             "而不是靠猜或全部重编。")
    args = parser.parse_args()

    test_path = Path(args.test_source)
    if not test_path.is_absolute():
        test_path = REPO_ROOT / test_path
    if not test_path.is_file():
        raise SystemExit(f"测试文件不存在：{test_path}")

    out_bin = f"/tmp/{test_path.stem}"

    extra_paths: list[Path] = []
    for raw in args.extra_source:
        p = Path(raw)
        if not p.is_absolute():
            p = REPO_ROOT / p
        if not p.is_file():
            raise SystemExit(f"--extra-source 文件不存在：{p}")
        extra_paths.append(p)

    cmd = build_command(args.template)
    # ZHeaderTool 的 *.generated.h 是构建产物，不入库；本脚本自己生成一份放 /tmp。
    cmd.append("-I/tmp/zgen_sa/inc")
    cmd += ["-o", out_bin, str(test_path)]
    cmd += [str(p) for p in extra_paths]
    link_dir = BIN_DIR
    if args.exclude_archive_member:
        link_dir = prepare_pruned_lib_dir(args.exclude_archive_member)
    cmd.append(f"-L{link_dir}")
    cmd.append(f"-F{BIN_DIR}")
    if args.link_map:
        cmd.append(f"-Wl,-map,{args.link_map}")
    # 静态库顺序敏感（依赖者在前、被依赖者在后），而我们无从知道真实依赖
    # 图 —— 列两遍是链接静态库时的常规兜底。
    for _ in range(2):
        for lib in LINK_LIBS:
            if (link_dir / f"lib{lib}.a").is_file():
                cmd.append(f"-l{lib}")
    if (BIN_DIR / "BqLog.framework").exists():
        cmd += ["-framework", "BqLog"]

    # Apple：只要链接器顺着某个 typeinfo（例如 typeid(WindowSystem)）拉进了
    # MacOSWindow.mm.o，就会一路要 Objective-C 运行时、Cocoa、乃至 Jolt 物理
    # —— 越补越多（补到第四层还没完）。
    #
    # 所以这里改成让未定义符号延迟到运行时解析：libZRuntime.a 是一个完整的
    # 构建产物，缺的只是**测试不会走到的**那些分支依赖的第三方库。测试只要
    # 不调用它们就不会解析，不会崩。真调用到了会 dyld 报错而不是静默出错。
    if sys.platform == "darwin":
        cmd.append("-Wl,-undefined,dynamic_lookup")
        for fw in ("Cocoa", "QuartzCore", "Metal", "IOKit", "CoreVideo", "CoreGraphics"):
            cmd += ["-framework", fw]

    print(f"[build] {test_path.name} ...", flush=True)
    r = subprocess.run(cmd, cwd=REPO_ROOT / "build", capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        return r.returncode

    print(f"\n[run] {out_bin}", flush=True)
    rr = subprocess.run([out_bin] + args.run_arg, capture_output=True, text=True)
    sys.stdout.write(rr.stdout)
    sys.stderr.write(rr.stderr)
    return rr.returncode


if __name__ == "__main__":
    sys.exit(main())
