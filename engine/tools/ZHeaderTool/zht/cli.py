"""Command-line driver.

    ZHeaderTool.py list     --headers-file F --output O
    ZHeaderTool.py generate --headers-file F --include-root R
                            --out-inc DIR --out-src DIR
                            --manifest M --stamp S [--module NAME] [--dump-ir J]

`list` runs at CMake configure time and writes the set of annotated headers
(one absolute path per line) so CMake knows which .gen.cpp files exist.

`generate` runs at build time. It re-scans every header of the module (so a
freshly annotated header is noticed), regenerates outputs, writes files only
when their content changed (Ninja `restat` keeps downstream TUs untouched),
prunes stale outputs and touches the stamp. If the annotated set no longer
matches the configure-time manifest, the manifest is rewritten (CMake watches
it via CMAKE_CONFIGURE_DEPENDS) and the step fails asking for one more build,
which re-configures automatically.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, List, Optional, Sequence, Tuple

from .codegen import generate_header, generate_source
from .model import HeaderInfo
from .parser import ParseError, has_annotations, parse_header

GENERATED_SUFFIXES = (".generated.h", ".gen.cpp")


def _norm(path: str) -> str:
    path = os.path.abspath(path)
    return path.replace("\\", "/")


def _key(path: str) -> str:
    # Case-insensitive file systems (Windows / default macOS) must not see
    # Foo.h and foo.h as different headers.
    return path.lower() if os.name == "nt" or sys.platform == "darwin" else path


def _diag(path: str, line: int, message: str) -> str:
    if os.name == "nt":
        return "%s(%d): error ZHT: %s" % (path.replace("/", "\\"), line, message)
    return "%s:%d: error: %s" % (path, line, message)


def _read_lines(path: str) -> List[str]:
    with open(path, "r", encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip()]


def _read_text(path: str) -> str:
    with open(path, "rb") as f:
        data = f.read()
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.decode("utf-8", errors="replace")


def write_if_changed(path: str, content: str) -> bool:
    data = content.encode("utf-8")
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return True


def scan_annotated(headers: Sequence[str]) -> List[str]:
    annotated = []
    for header in headers:
        try:
            with open(header, "rb") as f:
                data = f.read()
        except OSError:
            continue
        # Byte-level pre-filter: the vast majority of headers never mention
        # ZCLASS, so skip decoding + tokenizing them entirely.
        if b"ZCLASS" not in data:
            continue
        if has_annotations(_read_text(header)):
            annotated.append(header)
    return sorted(annotated, key=_key)


def _manifest_text(paths: Sequence[str]) -> str:
    return "".join(p + "\n" for p in paths)


def cmd_list(args) -> int:
    headers = [_norm(h) for h in _read_lines(args.headers_file)]
    annotated = scan_annotated(headers)
    write_if_changed(args.output, _manifest_text(annotated))
    return 0


def _include_path(header: str, include_root: str) -> Optional[str]:
    rel = os.path.relpath(header, include_root).replace("\\", "/")
    if rel.startswith("../") or rel == "..":
        return None
    return rel


def cmd_generate(args) -> int:
    include_root = _norm(args.include_root)
    headers = [_norm(h) for h in _read_lines(args.headers_file)]
    annotated = scan_annotated(headers)
    errors: List[str] = []

    parsed: List[HeaderInfo] = []
    for header in annotated:
        include_path = _include_path(header, include_root)
        if include_path is None:
            errors.append(_diag(header, 1, "header is outside the include root %s" % include_root))
            continue
        try:
            parsed.append(parse_header(header, include_path, _read_text(header)))
        except ParseError as exc:
            errors.append(_diag(header, exc.line, exc.message))

    # Generated files land in one flat directory per module, so base names
    # must be unique (same rule as UHT's "<Name>.generated.h").
    by_base: Dict[str, HeaderInfo] = {}
    for info in parsed:
        other = by_base.get(info.base_name.lower())
        if other is not None:
            errors.append(_diag(info.path, info.classes[0].line, "reflected header base name '%s' collides with %s" % (info.base_name, other.path)))
        else:
            by_base[info.base_name.lower()] = info

    seen_classes: Dict[str, Tuple[HeaderInfo, int]] = {}
    for info in parsed:
        for cls in info.classes:
            prev = seen_classes.get(cls.name)
            if prev is not None:
                errors.append(_diag(info.path, cls.line, "reflected class '%s' is also declared in %s:%d" % (cls.name, prev[0].path, prev[1])))
            else:
                seen_classes[cls.name] = (info, cls.line)

    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        print("ZHeaderTool[%s]: %d error(s)" % (args.module, len(errors)), file=sys.stderr)
        return 1

    expected_paths: Dict[str, str] = {}
    if os.path.exists(args.manifest):
        expected_paths = {_key(p): p for p in _read_lines(args.manifest)}
    actual_paths = {_key(p): p for p in annotated}
    expected = set(expected_paths)
    actual = set(actual_paths)

    written = 0
    produced = set()
    for info in parsed:
        h_path = os.path.join(args.out_inc, info.generated_header_name)
        s_path = os.path.join(args.out_src, info.generated_source_name)
        produced.add(_key(_norm(h_path)))
        produced.add(_key(_norm(s_path)))
        written += write_if_changed(h_path, generate_header(info))
        written += write_if_changed(s_path, generate_source(info))

    pruned = 0
    for out_dir in (args.out_inc, args.out_src):
        if not os.path.isdir(out_dir):
            continue
        for name in os.listdir(out_dir):
            if not name.endswith(GENERATED_SUFFIXES):
                continue
            full = _norm(os.path.join(out_dir, name))
            if _key(full) not in produced:
                os.remove(full)
                pruned += 1

    if args.dump_ir:
        write_if_changed(args.dump_ir, json.dumps([p.to_dict() for p in parsed], indent=2, sort_keys=True) + "\n")

    if actual != expected:
        write_if_changed(args.manifest, _manifest_text(annotated))
        added = sorted(actual_paths[k] for k in actual - expected)
        removed = sorted(expected_paths[k] for k in expected - actual)
        print(
            "ZHeaderTool[%s]: the set of ZCLASS headers changed (added: %s; removed: %s). "
            "The manifest was updated -- build again and CMake will re-configure automatically."
            % (args.module, ", ".join(added) or "-", ", ".join(removed) or "-"),
            file=sys.stderr,
        )
        return 2

    os.makedirs(os.path.dirname(args.stamp) or ".", exist_ok=True)
    with open(args.stamp, "w", encoding="utf-8") as f:
        f.write("%d headers, %d classes\n" % (len(parsed), sum(len(p.classes) for p in parsed)))

    if args.verbose:
        print("ZHeaderTool[%s]: %d reflected header(s), %d file(s) updated, %d stale file(s) removed" % (args.module, len(parsed), written, pruned))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="ZHeaderTool", description="ZEngine reflection code generator")
    sub = ap.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="write the list of ZCLASS-annotated headers")
    p_list.add_argument("--headers-file", required=True)
    p_list.add_argument("--output", required=True)
    p_list.set_defaults(func=cmd_list)

    p_gen = sub.add_parser("generate", help="generate .generated.h / .gen.cpp")
    p_gen.add_argument("--headers-file", required=True)
    p_gen.add_argument("--include-root", required=True)
    p_gen.add_argument("--out-inc", required=True)
    p_gen.add_argument("--out-src", required=True)
    p_gen.add_argument("--manifest", required=True)
    p_gen.add_argument("--stamp", required=True)
    p_gen.add_argument("--module", default="Module")
    p_gen.add_argument("--dump-ir", default="")
    p_gen.add_argument("--verbose", action="store_true")
    p_gen.set_defaults(func=cmd_generate)

    args = ap.parse_args(argv)
    return args.func(args)
