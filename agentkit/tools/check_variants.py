"""变体生成器的快速校验：只编译，不运行。

为什么需要这个
--------------
改 _wrap_depth 的一行代码，要靠跑完 44 个样本（约 105 秒）才能发现
"编译失败"。反馈周期长到让人倾向于相信第一次结果，而这恰恰是最危险的
时刻 —— 我就是在 d1/d2 全部失效时，先看到"top1 100%"才去查失败清单的。

编译校验把反馈压到几秒。变体是机器生成的，就该用机器能立刻判定的方式
验：能不能编译，是它成立的**必要条件**，而且判定成本极低。

只在 Windows + MSVC 下真正编译；其它平台只做静态检查并明确报告
"未验证"，不假装通过。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zfault.patterns import all_patterns             # noqa: E402
from zfault.variants import generate_variants        # noqa: E402


def _source_for_check(v) -> str:
    """
    取校验用源码：**必须走探针注入**。

    lab 编译时是 `inject_probe(pattern.source)`，而这里如果直接编译
    pattern.source，验的就是另一份代码 —— 探针注入会改 main 结构，
    变体失效恰恰发生在那一步。拿未注入的源码校验通过，等于没验。
    """
    try:
        from zlab.probe import inject_probe            # type: ignore
        return inject_probe(v.source)
    except Exception:                                  # noqa: BLE001
        return v.source


def _find_toolchain():
    """复用 zlab 的 Toolchain.detect()。

    别自己找 cl.exe：cl 必须通过 vcvars 拿到 INCLUDE/LIB 才能编译，
    否则连 <cstdio> 都找不到。zlab 早已解决这件事（含 vcvars 环境捕获），
    重新实现一遍只会得到"找到了 cl 但编译失败"的假象。
    """
    try:
        import zfault.toolchain as _tc                  # type: ignore
        tc = _tc.detect()                               # 模块级函数，非类方法
    except Exception:                                   # noqa: BLE001
        return None, None
    if not getattr(tc, "cxx", None):
        return None, None
    return tc.cxx, getattr(tc, "env", {}) or {}


def compile_one(src: str, tmp: Path, cxx: str, env: dict):
    """
    编译**并运行**单个变体。

    只编译是不够的：这一轮 24 个变体全部编译通过，却有 16 个运行不崩 ——
    main 被压成单行导致探针注入装到了函数体外面，编译照样过。
    "能编译" 只说明语法对，"能崩" 才说明变体真的成立。两者都验。
    """
    import os
    c = tmp / "v.cpp"
    c.write_text(src, encoding="utf-8")
    exe = tmp / "v.exe"
    run_env = dict(os.environ)
    run_env.update(env)
    # /Od 关优化：优化会把转发层内联掉，depth 维度就白做了
    p = subprocess.run(
        [cxx, "/nologo", "/Od", "/Zi", "/EHsc",
         "/Fe:" + str(exe),
         "/Fo:" + str(tmp / "v.obj"),
         "/Fd:" + str(tmp / "vc140.pdb"),
         str(c)],
        capture_output=True, text=True, timeout=120, env=run_env,
    )
    if p.returncode != 0 or not exe.exists():
        err = (p.stderr or p.stdout or "")[:400]
        return False, "编译失败: " + err

    # 运行：变体必须真的崩（0xC0000000 段），否则它对评估毫无贡献
    try:
        r = subprocess.run([str(exe)], capture_output=True,
                           timeout=30, env=run_env)
        rc = r.returncode
    except subprocess.TimeoutExpired:
        return False, "运行超时"
    except Exception as e:                                # noqa: BLE001
        return False, "运行异常: %s" % e

    if rc is None:
        return False, "未取得返回码"
    if (rc & 0xF0000000) == 0xC0000000:
        return True, ""
    return False, "未崩溃（rc=0x%X）—— 变体不成立" % (rc & 0xFFFFFFFF)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261014)
    a = ap.parse_args()

    variants = generate_variants(per_pattern=a.per, seed=a.seed)
    print("\n变体编译校验：%d 个变体" % len(variants))

    cxx, env = _find_toolchain()
    if not cxx:
        print("  未找到可用 C++ 编译器 —— 只能做静态检查，**未真正验证可编译性**")
        print("  静态检查：根因标记是否都在（不保证能编译）")
        bad = [v.id for v in variants if v.root_cause_line() < 0]
        for v in bad:
            print("    缺标记: %s" % v)
        print("  静态检查 %s" % ("通过" if not bad else "失败 %d" % len(bad)))
        return 1 if bad else 2

    ok, fail = 0, []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for v in variants:
            good, err = compile_one(_source_for_check(v), tmp, cxx, env)
            if good:
                ok += 1
            else:
                fail.append((v.id, err))

    print("  编译通过 %d/%d" % (ok, len(variants)))
    if fail:
        print("  失败 %d 个：" % len(fail))
        for vid, err in fail[:8]:
            print("    %s" % vid)
            for line in err.splitlines()[:3]:
                if "error" in line.lower():
                    print("      %s" % line.strip()[:110])
        if len(fail) > 8:
            print("    ... 另有 %d 个" % (len(fail) - 8))
        return 1
    print("  全部可编译")
    return 0


if __name__ == "__main__":
    sys.exit(main())
