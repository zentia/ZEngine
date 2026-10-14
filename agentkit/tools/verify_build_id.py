#!/usr/bin/env python3
"""
独立验证 PE Debug Directory 解析逻辑。

目的：CrashHandler.h 里的 ReadBuildIdentity() 是手工解析 PE 结构的，
这块最容易悄悄失效（RVA->文件偏移算错、section 越界、字节序搞反）。
本机 cl.exe 不可用，无法直接编译 C++ 验证，所以用 Python 按完全相同
的算法跑一遍真实 exe：如果 Python 版能稳定提取出 GUID+Age，且和
C++ 侧的算法步骤一一对应，就能确认算法本身是对的。

另外这也反向产出 ground truth，供 agentkit 侧测试使用。
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

IMAGE_DOS_SIGNATURE = 0x5A4D      # 'MZ'
IMAGE_NT_SIGNATURE = 0x00004550   # 'PE\0\0'
IMAGE_DIRECTORY_ENTRY_DEBUG = 6
IMAGE_DEBUG_TYPE_CODEVIEW = 2


def read_build_identity(path: Path) -> dict:
    """按 CrashHandler.h 里 ReadBuildIdentity 的同样步骤解析。"""
    data = path.read_bytes()
    out = {"build_id": "", "timestamp": 0, "ok": False, "error": ""}

    # DOS header
    if len(data) < 64 or struct.unpack_from("<H", data, 0)[0] != IMAGE_DOS_SIGNATURE:
        out["error"] = "not MZ"
        return out
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]

    # NT headers
    if struct.unpack_from("<I", data, e_lfanew)[0] != IMAGE_NT_SIGNATURE:
        out["error"] = "not PE"
        return out

    # FileHeader 在 e_lfanew+4 之后：Machine(2) NumberOfSections(2)
    # TimeDateStamp(4) PointerToSymbolTable(4) NumberOfSymbols(4)
    # SizeOfOptionalHeader(2) Characteristics(2)
    fh = e_lfanew + 4
    number_of_sections = struct.unpack_from("<H", data, fh + 2)[0]
    timestamp = struct.unpack_from("<I", data, fh + 4)[0]
    size_of_optional = struct.unpack_from("<H", data, fh + 16)[0]

    out["timestamp"] = timestamp

    # OptionalHeader 紧随 FileHeader(20 字节)
    opt = fh + 20
    # 判断 PE32 / PE32+：Magic 在 OptionalHeader 开头
    magic = struct.unpack_from("<H", data, opt)[0]
    is_pe32plus = magic == 0x20B

    # DataDirectory 起始偏移：
    #   PE32  : opt + 96
    #   PE32+ : opt + 112
    dd_base = opt + (112 if is_pe32plus else 96)
    entry_off = dd_base + IMAGE_DIRECTORY_ENTRY_DEBUG * 8
    debug_rva = struct.unpack_from("<I", data, entry_off)[0]
    debug_size = struct.unpack_from("<I", data, entry_off + 4)[0]

    out["is_pe32plus"] = is_pe32plus
    out["debug_rva"] = debug_rva
    out["debug_size"] = debug_size

    if debug_rva == 0 or debug_size < 28:
        out["error"] = "no debug directory"
        return out

    # Section headers 紧跟 OptionalHeader
    sec_base = opt + size_of_optional
    sections = []
    for i in range(number_of_sections):
        s = sec_base + i * 40
        name = data[s:s + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((name, vaddr, vsize, rawptr, rawsize))

    # RVA -> 文件偏移（走 VirtualSize，而不是 SizeOfRawData）
    file_off = None
    for name, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= debug_rva < vaddr + max(vsize, rawsize):
            file_off = rawptr + (debug_rva - vaddr)
            out["hit_section"] = name
            break

    if file_off is None:
        out["error"] = "debug rva not in any section"
        return out

    # 遍历 IMAGE_DEBUG_DIRECTORY（每个 28 字节）
    n = min(debug_size // 28, 16)
    for i in range(n):
        d = file_off + i * 28
        if d + 28 > len(data):
            break
        _chars, _ts, _maj, _min, dtype, dsize, drva, dptr = struct.unpack_from(
            "<IIHHIIII", data, d)
        if dtype != IMAGE_DEBUG_TYPE_CODEVIEW:
            continue
        if dptr == 0 or dptr + 24 > len(data):
            continue
        if data[dptr:dptr + 4] != b"RSDS":
            continue
        # RSDS: sig(4) GUID(16) Age(4) path
        guid_bytes = data[dptr + 4:dptr + 20]
        d1, d2, d3 = struct.unpack_from("<IHH", guid_bytes, 0)
        d4 = guid_bytes[8:16]
        age = struct.unpack_from("<I", data, dptr + 20)[0]

        guid_str = "%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x" % (
            d1, d2, d3, *d4)
        out["build_id"] = guid_str + ("%x" % age)
        out["age"] = age
        out["ok"] = True
        return out

    out["error"] = "no RSDS codeview record"
    return out


def main() -> int:
    if len(sys.argv) > 1:
        paths = [Path(p) for p in sys.argv[1:]]
    else:
        paths = [
            Path(r"E:\Engine\ZEngine\bin\Debug\ZEditor.exe"),
            Path(r"E:\Engine\ZEngine\bin\Release\ZEditor.exe"),
            Path(r"E:\Engine\ZEngine\bin\RelWithDebInfo\ZEditor.exe"),
        ]

    any_ok = False
    for p in paths:
        if not p.exists():
            print("%-52s MISSING" % p.name)
            continue
        r = read_build_identity(p)
        status = "OK " if r["ok"] else "FAIL"
        print("%-52s %s" % (str(p), status))
        if r["ok"]:
            any_ok = True
            print("    build_id : %s" % r["build_id"])
            print("    age      : %s" % r.get("age"))
            print("    timestamp: %s" % r["timestamp"])
            print("    pe32+    : %s  section=%s" % (r.get("is_pe32plus"),
                                                     r.get("hit_section")))
        else:
            print("    error    : %s (rva=0x%x size=%d)" % (
                r["error"], r.get("debug_rva", 0), r.get("debug_size", 0)))

    return 0 if any_ok else 1


if __name__ == "__main__":
    sys.exit(main())
