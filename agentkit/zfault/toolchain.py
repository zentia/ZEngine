"""
工具链探测。

设计原则：**探测而不是假设**。

早期版本硬编码用 g++，结果在一台机器上它指向 Strawberry Perl 附带的 g++ 4.7.3
（连 C++11 都不完整），编译现代代码必挂。教训是：
`which g++` 存在不等于可用，必须实际编译一个探针程序验证能力。
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# 探针：用到 C++14 特性，任何太老的编译器都会编不过
_PROBE = r"""
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
int main() {
    auto v = std::make_unique<std::vector<std::string>>();
    v->push_back("ok");
    std::printf("%s\n", v->at(0).c_str());
    return 0;
}
"""


@dataclass
class Toolchain:
    name: str = ""                    # msvc / gcc / clang / none
    cxx: str = ""                     # 编译器可执行文件
    version: str = ""
    addr2line: str = ""
    nm: str = ""
    supported: bool = False
    notes: list[str] = field(default_factory=list)
    env: dict[str, str] = field(default_factory=dict)  # 构建所需环境（如 vcvars 结果）

    @property
    def available(self) -> bool:
        return self.supported

    @property
    def build_env(self) -> dict[str, str] | None:
        return self.env or None

    def __str__(self) -> str:
        if not self.supported:
            return f"[不可用] {self.name or 'none'}"
        return f"[{self.name}] {self.cxx} ({self.version})"


def _run(cmd: list[str], timeout: int = 60,
         env: dict[str, str] | None = None,
         shell: bool = False) -> tuple[int, str, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           env=env, shell=shell, errors="replace")
        return p.returncode, p.stdout or "", p.stderr or ""
    except (subprocess.TimeoutExpired, OSError) as e:
        return 1, "", str(e)


def _probe_compile(cxx: str, cxxflags: list[str] | None = None,
                   env: dict[str, str] | None = None) -> tuple[bool, str]:
    """实际编译并运行探针。能跑出 'ok' 才算可用。"""
    with tempfile.TemporaryDirectory(prefix="zfl_probe_") as d:
        src = Path(d) / "probe.cpp"
        exe = Path(d) / ("probe.exe" if _is_windows() else "probe")
        src.write_text(_PROBE, encoding="utf-8")

        cmd = [cxx, str(src), "-o", str(exe)]
        # 探针只用到 C++14，但要求在 C++17 下也能过——太老的编译器在这里就露馅
        cmd += ["-std=c++17"]
        if cxxflags:
            cmd += cxxflags

        rc, _, err = _run(cmd, timeout=120, env=env)
        if rc != 0:
            return False, (err or "").splitlines()[0] if err else "编译失败"

        rc2, out, _ = _run([str(exe)], timeout=30, env=env)
        if rc2 != 0 or "ok" not in out:
            return False, "编译成功但运行失败"

    return True, ""


def _is_windows() -> bool:
    import platform
    return platform.system() == "Windows"


def _version_of(cxx: str) -> str:
    rc, out, err = _run([cxx, "--version"])
    text = ((out or "") + (err or "")).strip()
    return text.splitlines()[0][:80] if text else "unknown"


def _find_msvc() -> tuple[str, list[str]]:
    """
    定位 MSVC。cl 一般不在 PATH 里，需要按 VS 安装布局找，
    且必须通过 vcvars 拿到 INCLUDE/LIB 才能编译——所以这里同时返回 vcvars 路径。
    """
    import os

    # vswhere 固定在 ProgramFiles(x86)，但 VS 本体可能在 ProgramFiles——
    # 两者不能混用，必须用 vswhere 返回的 installationPath 作为根目录。
    roots = [os.environ.get("ProgramFiles(x86)", ""), os.environ.get("ProgramFiles", "")]
    vswhere = ""
    for r in roots:
        if not r:
            continue
        cand = Path(r) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if cand.exists():
            vswhere = str(cand)
            break

    if not vswhere:
        return "", []

    rc, out, _ = _run([str(vswhere), "-latest", "-property", "installationPath"])
    if rc != 0 or not out.strip():
        return "", []

    root = Path(out.strip().splitlines()[0])
    msvc_root = root / "VC" / "Tools" / "MSVC"
    if not msvc_root.exists():
        return "", []

    versions = sorted([p for p in msvc_root.iterdir() if p.is_dir()],
                      key=lambda p: p.name, reverse=True)
    import platform
    host = "Hostx64" if platform.machine().endswith("64") else "Hostx86"
    target = "x64" if platform.machine().endswith("64") else "x86"

    for v in versions:
        cl = v / "bin" / host / target / "cl.exe"
        if cl.exists():
            # 优先 vcvarsall（它按参数选择目标架构，比 vcvars64 更可靠）
            bld = root / "VC" / "Auxiliary" / "Build"
            vcvarsall = bld / "vcvarsall.bat"
            if vcvarsall.exists():
                return str(cl), [str(vcvarsall), target]
            vcvars = bld / ("vcvars64.bat" if target == "x64" else "vcvars32.bat")
            if vcvars.exists():
                return str(cl), [str(vcvars)]
            return str(cl), []
    return "", []


def _vcvars_env(vcvars: list[str] | str) -> dict[str, str] | None:
    """
    运行 vcvars 并捕获它设置的环境变量。

    cl 必须依赖 vcvars 设置的 INCLUDE/LIB/PATH，
    否则连 <cstdio> 都找不到。做法：跑 vcvars 后再 `set`，解析输出。
    """
    import os

    # 兼容两种形态：[vcvars.bat] 或 [vcvarsall.bat, <arch>]
    parts = [vcvars] if isinstance(vcvars, str) else list(vcvars)
    arch = parts[1] if len(parts) > 1 else ""
    script = parts[0]

    # 注意：不要用 `>nul 2>&1 && set` 这种写法。
    # 实测在 cmd /c 下该组合会返回 1 且不产生输出（重定向与 && 的解析问题），
    # 导致误判为「vcvars 不可用」。去掉重定向后正常返回 116 行环境变量。
    cmd: list[str] = ["cmd", "/c", script]
    if arch:
        cmd.append(arch)
    cmd += ["&&", "set"]

    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=180,
                           errors="replace")
    except (subprocess.TimeoutExpired, OSError):
        return None

    if not (p.stdout or "").strip():
        return None

    env: dict[str, str] = dict(os.environ)
    for line in (p.stdout or "").splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            k = k.strip()
            if k:
                env[k] = v

    # 以 vcvars 的变量为准，但保留父进程里它没覆盖的项
    # （SystemRoot、TEMP 等缺失会让子进程直接起不来）
    return env or None


def detect() -> Toolchain:
    """
    探测本机可用工具链。按 msvc > clang > gcc 顺序，找到第一个**实际能编译**的。

    返回 supported=False 时，调用方应降级到「不编译」模式，而不是继续硬跑。
    """
    tc = Toolchain()

    # --- 符号化工具（不决定可用性，只记录）---
    for name in ("llvm-addr2line", "addr2line"):
        p = shutil.which(name)
        if p:
            tc.addr2line = p
            break
    for name in ("llvm-nm", "nm"):
        p = shutil.which(name)
        if p:
            tc.nm = p
            break

    # --- 编译器候选 ---
    candidates: list[tuple[str, str, list[str]]] = []

    cl, vcvars_list = _find_msvc()
    cl_env: dict[str, str] | None = None
    if cl:
        if vcvars_list:
            # 必须传完整列表：vcvarsall 需要架构参数（x64），
            # 只传脚本名会以默认架构初始化，与 cl 的目标架构不匹配。
            cl_env = _vcvars_env(vcvars_list)
            if cl_env is None:
                tc.notes.append("找到 cl.exe 但 vcvars 未能生成环境，MSVC 不可用")
        candidates.append(("msvc", cl, [], cl_env))

    for exe in ("clang++", "g++", "c++"):
        p = shutil.which(exe)
        if p:
            name = "clang" if "clang" in exe else "gcc"
            candidates.append((name, p, [], None))

    if not candidates:
        tc.notes.append("未找到任何 C++ 编译器")
        return tc

    # --- 逐个实测 ---
    for name, cxx, flags, env in candidates:
        if name == "msvc":
            ok, why = _probe_msvc(cxx, env)
        else:
            ok, why = _probe_compile(cxx, flags, env)

        if ok:
            tc.name = name
            tc.cxx = cxx
            tc.version = _version_of(cxx) if name != "msvc" else "msvc " + _msvc_ver(cxx)
            tc.supported = True
            tc.env = env or {}
            return tc

        tc.notes.append(f"{name} ({Path(cxx).name}) 不可用: {why}")

    tc.notes.append("所有候选编译器均未通过能力探针")
    return tc


def _probe_dir() -> str:
    """
    探针编译目录。固定放在工程内，且每次复用同一路径。

    不能用 tempfile.mkdtemp()：系统 Temp 下生成的 exe 会被防病毒
    实时扫描锁住句柄，TemporaryDirectory 退出时清理报 PermissionError，
    导致整个工具链探测失败。
    """
    d = Path(__file__).resolve().parent.parent / "_lab_work" / "_probe"
    d.mkdir(parents=True, exist_ok=True)
    return str(d)


def _probe_msvc(cl: str, env: dict[str, str] | None = None) -> tuple[bool, str]:
    """
    MSVC 探针：cl 必须在 vcvars 环境下才能找到标准库头文件。

    这里曾经漏传 env，导致 cl 报「找不到 <cstdio>」而误判 MSVC 不可用——
    实际上编译器是好的，只是缺 INCLUDE/LIB。
    """
    # 不用 tempfile.mkdtemp()：系统 Temp 目录下生成的 exe 会被防病毒
    # 实时扫描锁住，TemporaryDirectory 清理时报 PermissionError，
    # 整个探测随之失败。实测这一条会让基线脚本直接崩掉。
    d = _probe_dir()
    src = Path(d) / "probe.cpp"
    src.write_text(_PROBE, encoding="utf-8")
    exe = Path(d) / "probe.exe"
    obj = Path(d) / "probe.obj"

    rc, _, err = _run(
        [cl, "/nologo", "/std:c++17", "/EHsc", "/Zi",
         f"/Fe:{exe}", f"/Fo:{obj}", str(src)],
        timeout=180, env=env,
    )
    if rc != 0:
        first = next((l for l in (err or "").splitlines() if l.strip()), "cl 编译失败")
        return False, first[:120]

    rc2, out, _ = _run([str(exe)], timeout=30, env=env)
    if rc2 != 0 or "ok" not in (out or ""):
        return False, "编译成功但运行失败"

    return True, ""


def _msvc_ver(cl: str) -> str:
    """从路径里提取 MSVC 工具集版本，如 14.39.33519。"""
    for part in Path(cl).parts:
        if part[0].isdigit() and part.count(".") >= 2:
            return part
    return "unknown"


def require() -> Toolchain:
    """探测并校验，不可用直接抛异常。"""
    tc = detect()
    if not tc.supported:
        detail = "; ".join(tc.notes) or "无可用编译器"
        raise RuntimeError(f"未找到可用的 C++ 工具链: {detail}")
    return tc
