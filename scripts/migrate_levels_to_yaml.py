#!/usr/bin/env python3
"""Migrate legacy ZEngine JSON levels to the block-YAML .scene format.

Legacy levels are `<name>.level.json`. They deserialise through
`LevelRes::Transfer`, where `m_Objects` is commented out -- so a JSON level
produces zero runtime GameObjects. The `.scene` branch (`ReadObjectsFromYaml`)
works and is what the editor writes, so migration is the only way to make an
old level loadable again.

The conversion itself is done by the engine's own `SceneGenTool` (built from
`engine/Source/Runtime/Function/AI/Test`). Doing it in-engine guarantees the
output cannot drift from what `Level::load` expects.

Usage:
    python migrate_levels_to_yaml.py --project <dir> [--dry-run] [--keep-json]

    --project     Project root (the directory holding the .zproject), or the
                  path to the .zproject itself.
    --dry-run     Report what would change; write nothing.
    --keep-json   Keep the original .json after converting (default: delete
                  it only after the .scene verifies non-empty).
    --tool        Explicit path to SceneGenTool[.exe]; auto-detected if absent.

Exit code 0 on success, 1 if any level failed.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def find_tool(explicit: str | None) -> Path | None:
    """Locate the SceneGenTool executable."""
    if explicit:
        candidate = Path(explicit)
        return candidate if candidate.is_file() else None

    roots = [Path("E:/Engine/ZEngine/bin/Debug"), Path("E:/Engine/ZEngine/bin/Release")]
    for root in roots:
        for name in ("SceneGenTool.exe", "SceneGenTool"):
            candidate = root / name
            if candidate.is_file():
                return candidate
    return None


def find_levels(project_root: Path) -> list[Path]:
    """Collect every legacy .level.json under the project."""
    return sorted(project_root.rglob("*.level.json"))


def migrate(level_json: Path, tool: Path, dry_run: bool, keep_json: bool) -> bool:
    """Convert one .level.json to .scene. Returns True on success."""
    scene_path = level_json.with_suffix("")  # strip .json -> <name>.level
    scene_path = scene_path.with_suffix(".scene")  # strip .level -> <name>.scene

    if scene_path.exists():
        print(f"  skip  {level_json.name}: {scene_path.name} already exists")
        return True

    if dry_run:
        print(f"  would {level_json.name} -> {scene_path.name}")
        return True

    result = subprocess.run(
        [str(tool), str(level_json), str(scene_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  FAIL  {level_json.name}: SceneGenTool exit {result.returncode}")
        if result.stderr:
            print(f"        {result.stderr.strip()[:200]}")
        return False

    if not scene_path.is_file() or scene_path.stat().st_size == 0:
        print(f"  FAIL  {level_json.name}: produced empty/missing {scene_path.name}")
        return False

    print(f"  ok    {level_json.name} -> {scene_path.name}")

    if not keep_json:
        # Only remove the source once the .scene is verified on disk, so a
        # failed conversion never costs the user their only copy.
        level_json.unlink()
        print(f"  rm    {level_json.name}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", required=True, help="Project root or .zproject path")
    parser.add_argument("--dry-run", action="store_true", help="Report changes without writing")
    parser.add_argument("--keep-json", action="store_true", help="Keep the source .json files")
    parser.add_argument("--tool", default=None, help="Explicit SceneGenTool path")
    args = parser.parse_args()

    project = Path(args.project)
    if project.is_file():
        project = project.parent
    if not project.is_dir():
        print(f"error: project directory not found: {project}", file=sys.stderr)
        return 1

    levels = find_levels(project)
    if not levels:
        print(f"no legacy .level.json found under {project}")
        return 0

    tool = find_tool(args.tool)
    if tool is None:
        print("error: SceneGenTool not found. Build it or pass --tool <path>.", file=sys.stderr)
        return 1

    print(f"migrating {len(levels)} level(s) under {project} using {tool}")
    failures = 0
    for level in levels:
        if not migrate(level, tool, args.dry_run, args.keep_json):
            failures += 1

    if failures:
        print(f"\n{failures} level(s) failed; sources were left untouched for those.")
        return 1

    print("\nall levels migrated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
