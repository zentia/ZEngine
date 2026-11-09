"""ZEngine 参数化棋盘：spec -> 共享几何二进制 (ZBRD) -> three.js / Unity 双端消费。

设计要点
--------
* 两份真源，职责严格分离：
  - `board-spec.json` = **冻结核心**：连续台面的轮廓 / 逻辑网格落点数值 /
    包边 / 功能锚点 / 天穹 / LOD 预算 / 装饰槽位边界。AI 不得改动。
  - `style-pack.json` = **可变风格**：装饰物与交互物的数量、形态、位置、
    颜色、LOD、色板。可整包替换（人工或 AI 生成），但只能落在槽位内。
  逻辑 7x8 网格**不是** 56 个可见格子；现网皮肤核心区是一整块连续台面。
* 改 spec 用 `--set`，换风格用 `--style`，两端消费同一份 ZBRD，无需改 three.js 或 C#。
* 几何公式只在本文件实现一次，产出 `ZBRD` 后由 three.js 与 Unity 读同一份字节，
  避免两套公式漂移（A7 跨宿主一致性的前提）。
* 坐标：three.js Y-up / 右手系 / 1 单位 = 1 米。Unity 侧消费时统一做
  x -> -x、normal.x -> -normal.x、反转三角形绕序、uv.y -> 1 - uv.y。
* 网格数值来自 OSGame trunk 的 `W.玩法模板参数表.xlsx` 行 4（模板 4 / 交错矩形）
  与 `CrossRectangleUtil.cs:19-45`，不是估算值。

ZBRD v1（全部 little-endian，无对齐）::

    u32  magic       = 0x4452425A  ("ZBRD")
    u32  version     = 1
    u32  part_count
    u32  flags       = 0
    per part:
      u32 name_len      + utf8 bytes
      u8  kind          (0 grid / 1 frame / 2 sky / 3 decoration / 4 interactable)
      u8  lod_mask      (bit0 HD / bit1 High / bit2 Mid / bit3 Low)
      u8  pad, u8  pad
      u32 material_len  + utf8 bytes
      u32 tag_len       + utf8 bytes (interactable 才有)
      u32 vertex_count
      u32 index_count
      f32 matrix[16]    (row-major；几何已烘焙，恒为单位矩阵，留作扩展)
      f32 color[4]
      f32 positions[3 * vertex_count]
      f32 normals[3 * vertex_count]
      f32 uvs[2 * vertex_count]
      u32 indices[index_count]   (可重复引用同一顶点；不要求等于 vertex_count)
"""

# v1 早期所有部件都是「三角形汤」(index_count == vertex_count、indices=0..n-1)，
# 那只是 Python 生成器当时的实现选择，不是字节格式约束。现外部 glb 缓存改为
# 保留 glb 自带的索引网格以省内存；核心几何仍是三角形汤，两者都被消费端支持。

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib
import json
import math
import os
import struct
from dataclasses import dataclass, field
from typing import Any, Dict, List, Sequence, Tuple, cast

MAGIC = 0x4452425A
VERSION = 1

KIND_GRID = 0
KIND_FRAME = 1
KIND_SKY = 2
KIND_DECORATION = 3
KIND_INTERACTABLE = 4
KIND_NAMES = {
    KIND_GRID: "grid",
    KIND_FRAME: "frame",
    KIND_SKY: "sky",
    KIND_DECORATION: "decoration",
    KIND_INTERACTABLE: "interactable",
}

LOD_BITS = {"HD": 1, "High": 2, "Mid": 4, "Low": 8}
LOD_RANK = {"HD": 0, "High": 1, "Mid": 2, "Low": 3}
ALL_LODS = 0b1111

Vec3 = Tuple[float, float, float]
Vec2 = Tuple[float, float]


class SpecError(ValueError):
    """spec 非法。"""


class ZBoardError(ValueError):
    """ZBRD 编解码错误。"""


# --------------------------------------------------------------------------
# 小工具
# --------------------------------------------------------------------------

def _sub(a: Sequence[float], b: Sequence[float]) -> Tuple[float, float, float]:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(a: Sequence[float]) -> Vec3:
    length = math.sqrt(sum(v * v for v in a)) or 1.0
    return (a[0] / length, a[1] / length, a[2] / length)


def _clamp_color(value: Sequence[float]) -> Tuple[float, float, float, float]:
    if len(value) not in (3, 4):
        raise SpecError("color must be 3 or 4 floats")
    rgba = list(value[:3]) + ([value[3]] if len(value) == 4 else [1.0])
    if any(not isinstance(v, (int, float)) or not 0.0 <= float(v) <= 1.0
           for v in rgba):
        raise SpecError("color channels must be floats in [0, 1]")
    return (float(rgba[0]), float(rgba[1]), float(rgba[2]), float(rgba[3]))


def _rotate_y(point: Sequence[float], degrees: float) -> Vec3:
    if not degrees:
        return (point[0], point[1], point[2])
    rad = math.radians(degrees)
    c, s = math.cos(rad), math.sin(rad)
    return (point[0] * c + point[2] * s, point[1],
            -point[0] * s + point[2] * c)


def _sha256_field(value: Any, where: str) -> None:
    if (not isinstance(value, str) or len(value) != 64
            or any(c not in "0123456789abcdef" for c in value)):
        raise SpecError("%s must be a lowercase sha256" % where)


def package_relative(value: Any, where: str) -> str:
    """契约里的资产路径只能是「包内相对路径」，不允许绝对路径或 .. 逃逸。"""
    if (not isinstance(value, str) or not value or value.startswith("/")
            or ".." in value.replace("\\", "/").split("/")
            or os.path.isabs(value)):
        raise SpecError("%s must be a package-relative path" % where)
    return os.path.join(*value.replace("\\", "/").split("/"))


# --------------------------------------------------------------------------
# spec
# --------------------------------------------------------------------------

def load_spec(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        spec = json.load(handle)
    validate_spec(spec)
    return spec


STYLE_SCHEMA = "zengine.chessboard.style-pack/v1"
VISUAL_SCHEMA = "zengine.chessboard.visual-pack/v1"
DEFAULT_COLOR = (0.5, 0.5, 0.5, 1.0)
VISUAL_SECTIONS = ("concept", "palette", "skybox", "post_processing",
                   "materials", "volume", "lighting")


def empty_style_pack() -> Dict[str, Any]:
    """只留冻结核心几何，不生成任何风格化部件。"""
    return {"schema": STYLE_SCHEMA, "name": "(none)",
            "generated_by": "none", "decorations": [], "interactables": []}


def empty_visual_pack() -> Dict[str, Any]:
    """只留冻结核心视觉（spec 自带），不引入 AI 视觉层。"""
    return {"schema": VISUAL_SCHEMA, "name": "(none)",
            "generated_by": "none", "materials": {}}


def validate_spec(spec: Dict[str, Any]) -> None:
    if spec.get("schema") != "zengine.chessboard.spec/v1":
        raise SpecError("unsupported spec schema")

    core = spec.get("core_contract")
    if (not isinstance(core, dict) or not core.get("fixed")
            or core.get("visual_kind") != "continuous_slab"
            or core.get("logical_grid_is_visual") is not False):
        raise SpecError("core_contract must declare a fixed continuous_slab "
                        "with logical_grid_is_visual=false")

    grid = spec.get("grid")
    if not isinstance(grid, dict):
        raise SpecError("missing `grid`")
    for key in ("columns", "rows", "cell_width_m", "cell_height_m",
                "margin_m", "thickness_m"):
        if not isinstance(grid.get(key), (int, float)):
            raise SpecError("missing or non-numeric `grid.%s`" % key)
    if int(grid["columns"]) < 1 or int(grid["rows"]) < 1:
        raise SpecError("grid.columns / grid.rows must be >= 1")
    if float(grid["cell_width_m"]) <= 0 or float(grid["cell_height_m"]) <= 0:
        raise SpecError("cell sizes must be > 0")
    if float(grid["margin_m"]) < 0 or float(grid["thickness_m"]) <= 0:
        raise SpecError("margin_m must be >= 0 and thickness_m > 0")
    if grid.get("layout") not in ("cross_rectangle", "align_rectangle"):
        raise SpecError("layout must be cross_rectangle or align_rectangle")
    if grid.get("shape") not in ("rect", "chamfer", "hex"):
        raise SpecError("unknown grid.shape (slab outline)")
    if grid.get("cell_marks") not in ("none", "engraved"):
        raise SpecError("grid.cell_marks must be none or engraved")
    offset = grid.get("center_offset_m")
    if not isinstance(offset, list) or len(offset) != 3:
        raise SpecError("grid.center_offset_m must be [x, y, z]")
    _clamp_color(grid["slab_color"])

    for section in ("frame", "sky", "lighting", "unity", "anchors",
                    "decoration_zones", "scene"):
        if not isinstance(spec.get(section), dict):
            raise SpecError("missing `%s`" % section)
    scene = spec["scene"]
    for key in ("path", "quality", "artist_root", "field_node"):
        if not scene.get(key):
            raise SpecError("scene.%s is required" % key)
    if scene["quality"] != "HD":
        raise SpecError("only the HD scene is delivered; lower tiers come from "
                        "OSGame's auto-downgrade tools")
    if (not str(scene["path"]).startswith("Assets/Scenes/pvp/")
            or not str(scene["path"]).endswith(".unity")):
        raise SpecError("scene.path must be an Assets/Scenes/pvp/*.unity scene")
    if scene.get("template") and not str(scene["template"]).endswith(".unity"):
        raise SpecError("scene.template must be a .unity scene path or null")
    unity = spec["unity"]
    if not unity.get("material_template"):
        raise SpecError("unity.material_template is required (custom SRP needs a real material)")
    if not isinstance(unity.get("color_properties"), list) or not unity["color_properties"]:
        raise SpecError("unity.color_properties must be a non-empty list")
    if float(spec["sky"]["radius_m"]) <= 0:
        raise SpecError("sky.radius_m must be > 0")
    if spec["sky"].get("mode") not in ("procedural", "panorama"):
        raise SpecError("sky.mode must be procedural or panorama")

    for key in ("items",):
        if not isinstance(spec["anchors"].get(key), list):
            raise SpecError("anchors.%s must be a list" % key)
        if not isinstance(spec["decoration_zones"].get(key), list):
            raise SpecError("decoration_zones.%s must be a list" % key)
    anchor_ids: set[str] = set()
    for anchor in spec["anchors"]["items"]:
        for key in ("id", "position", "radius_m", "height_m", "color"):
            if key not in anchor:
                raise SpecError("anchor missing `%s`" % key)
        if anchor["id"] in anchor_ids:
            raise SpecError("duplicate anchor id `%s`" % anchor["id"])
        anchor_ids.add(anchor["id"])
        if not isinstance(anchor["position"], list) or len(anchor["position"]) != 3:
            raise SpecError("anchor `%s` needs position [x, y, z]" % anchor["id"])
        _clamp_color(anchor["color"])
    # 2026-11-06 用户纠正：核心只冻结「一个方格 + 两个棋手占位」，
    # 占位必须落在左下 / 右上对角；草图里的中线、圆、小圆一概不是核心元素。
    seats = spec["anchors"]["items"]
    if len(seats) != 2 or {a.get("corner") for a in seats} != {
            "south_west", "north_east"}:
        raise SpecError("core contract requires exactly 2 player seats at the "
                        "south-west / north-east diagonal")
    a, b = (seat["position"] for seat in seats)
    if (float(a[0]) * float(b[0]) >= 0 or float(a[2]) * float(b[2]) >= 0):
        raise SpecError("the two player seats must be diagonal (opposite x and z signs)")
    zone_ids: set[str] = set()
    for zone in spec["decoration_zones"]["items"]:
        for key in ("id", "center", "half", "max_items"):
            if key not in zone:
                raise SpecError("decoration zone missing `%s`" % key)
        if zone["id"] in zone_ids:
            raise SpecError("duplicate decoration zone id `%s`" % zone["id"])
        zone_ids.add(zone["id"])
        if (not isinstance(zone["center"], list) or len(zone["center"]) != 2
                or not isinstance(zone["half"], list) or len(zone["half"]) != 2):
            raise SpecError("zone `%s` needs center [x, z] and half [hx, hz]" % zone["id"])
        if int(zone["max_items"]) < 0:
            raise SpecError("zone `%s` max_items must be >= 0" % zone["id"])

    lods = spec.get("lods")
    if not isinstance(lods, dict) or any(name not in lods for name in LOD_BITS):
        raise SpecError("lods must define HD / High / Mid / Low")
    if any("max_decorations" not in lods[name] for name in LOD_BITS):
        raise SpecError("each LOD needs max_decorations budget")

    # 风格相关内容必须放在 style-pack.json，不许塞回冻结的 core spec。
    if "decorations" in spec or "interactables" in spec:
        raise SpecError("style-dependent decorations/interactables belong in "
                        "style-pack.json, not in the frozen core spec")


EXTERNAL_MESH_SCHEMA = "zengine.chessboard.external-mesh/v1"
EXTERNAL_MESH_STATUSES = ("pending", "reference_ready", "asset_ready")
# 资产来源：代理几何（离线生成，用于先打通管线）/ 混元 3D / 人工制作。
ASSET_ORIGINS = ("proxy_zbrd", "hunyuan_3d", "handmade")


def validate_external_mesh_contract(pack: Dict[str, Any],
                                    spec: Dict[str, Any],
                                    root: str | None = None) -> None:
    """冻结的「独立 glb 资产 + 锚点引用」契约：只校验契约，不加载任何网格。

    混元 3D 的产物不烘进 ZBRD 共享几何：ZBRD 里的 part 仍保持 primitive 与
    `zboard_<id>` 材质名；glb 作为独立资产，按这里声明的 anchor 摆放。
    anchor 必须与风格包里对应项的 position 完全一致，避免替换时错位；
    `root` 为本文件所在目录，传入时还会逐字节校验参考图 / glb 的 sha256。
    """
    contract = pack.get("external_mesh_contract")
    if contract is None:
        return
    if contract.get("schema") != EXTERNAL_MESH_SCHEMA:
        raise SpecError("unsupported external_mesh_contract schema")
    if contract.get("frozen") is not True or contract.get("asset_format") != "glb":
        raise SpecError("external_mesh_contract must be frozen and use glb")
    if contract.get("reference_root") != "package_root":
        raise SpecError("external_mesh_contract.reference_root must be package_root")
    if not isinstance(contract.get("reference_images_deployed"), bool):
        raise SpecError("external_mesh_contract.reference_images_deployed must be a bool")
    items = contract.get("items")
    if not isinstance(items, list) or not items:
        raise SpecError("external_mesh_contract.items must be a non-empty list")
    zones = {zone["id"]: zone for zone in spec["decoration_zones"]["items"]}
    placed = {item["id"]: item for section in ("decorations", "interactables")
              for item in pack.get(section, [])}
    for style_id, source in placed.items():
        if not isinstance(source.get("theme_asset"), str) or not source["theme_asset"]:
            raise SpecError("style item `%s` needs theme_asset" % style_id)
    asset_ids: set[str] = set()
    covered: set[str] = set()

    def hash_of(relative: str, digest: str, where: str) -> None:
        if root is None:
            return
        path = os.path.join(root, relative)
        if not os.path.isfile(path):
            raise SpecError("missing %s" % where)
        with open(path, "rb") as handle:
            actual = hashlib.sha256(handle.read()).hexdigest()
        if actual != digest:
            raise SpecError("%s sha256 mismatch" % where)

    for entry in items:
        if not isinstance(entry, dict):
            raise SpecError("external_mesh_contract item must be an object")
        asset_id = entry.get("asset_id")
        if not isinstance(asset_id, str) or not asset_id or asset_id in asset_ids:
            raise SpecError("duplicate / empty external asset_id")
        asset_ids.add(asset_id)
        if entry.get("status") not in EXTERNAL_MESH_STATUSES:
            raise SpecError("bad status in external asset `%s`" % asset_id)

        image = entry.get("reference_image")
        digest = entry.get("reference_sha256")
        if image is None or digest is None:
            if entry["status"] == "pending":
                if image is not None or digest is not None:
                    raise SpecError("`%s` pending must not declare a partial reference"
                                    % asset_id)
            else:
                raise SpecError("`%s` needs reference_image + sha256" % asset_id)
        else:
            image = package_relative(image, "`%s` reference_image" % asset_id)
            _sha256_field(digest, "`%s` reference_sha256" % asset_id)
            hash_of(image, cast(str, digest),
                    "reference image for `%s`" % asset_id)
        size = entry.get("reference_size_px")
        if size is not None and (not isinstance(size, list) or len(size) != 2
                                 or any(not isinstance(v, int) or isinstance(v, bool)
                                        or v <= 0 for v in size)):
            raise SpecError("`%s` reference_size_px must be [w, h]" % asset_id)
        pivot = entry.get("pivot_offset")
        if pivot is not None and (not isinstance(pivot, list) or len(pivot) != 3
                                  or any(not isinstance(v, (int, float))
                                         or isinstance(v, bool) for v in pivot)):
            raise SpecError("`%s` pivot_offset must be [x, y, z]" % asset_id)

        mesh = entry.get("mesh")
        mesh_digest = entry.get("mesh_sha256")
        cache = entry.get("cache")
        cache_digest = entry.get("cache_sha256")
        if entry["status"] == "asset_ready":
            mesh = package_relative(mesh, "`%s` mesh" % asset_id)
            _sha256_field(mesh_digest, "`%s` mesh_sha256" % asset_id)
            hash_of(mesh, cast(str, mesh_digest), "mesh for `%s`" % asset_id)
            if entry.get("asset_origin") not in ASSET_ORIGINS:
                raise SpecError("`%s` asset_ready needs asset_origin in %s"
                                % (asset_id, ", ".join(ASSET_ORIGINS)))
            if (cache is None) != (cache_digest is None):
                raise SpecError("`%s` cache / cache_sha256 must be paired" % asset_id)
            if cache is not None:
                cache = package_relative(cache, "`%s` cache" % asset_id)
                _sha256_field(cache_digest, "`%s` cache_sha256" % asset_id)
                hash_of(cache, cast(str, cache_digest),
                        "mesh cache for `%s`" % asset_id)
            texture = entry.get("texture")
            texture_digest = entry.get("texture_sha256")
            if (texture is None) != (texture_digest is None):
                raise SpecError("`%s` texture / texture_sha256 must be paired"
                                % asset_id)
            if texture is not None:
                texture = package_relative(texture, "`%s` texture" % asset_id)
                _sha256_field(texture_digest, "`%s` texture_sha256" % asset_id)
                hash_of(texture, cast(str, texture_digest),
                        "baseColor texture for `%s`" % asset_id)
        elif (mesh is not None or mesh_digest is not None
              or entry.get("texture") is not None
              or entry.get("texture_sha256") is not None):
            raise SpecError("`%s` cannot declare a mesh before asset_ready" % asset_id)
        elif entry.get("asset_origin") is not None:
            raise SpecError("`%s` cannot declare asset_origin before asset_ready"
                            % asset_id)

        instances = entry.get("instances")
        if not isinstance(instances, list) or not instances:
            raise SpecError("`%s` needs instances" % asset_id)
        for instance in instances:
            if not isinstance(instance, dict):
                raise SpecError("bad instance in `%s`" % asset_id)
            style_id = instance.get("id")
            if style_id not in placed or style_id in covered:
                raise SpecError("unknown / duplicate instance `%s`" % style_id)
            covered.add(cast(str, style_id))
            source = placed[style_id]
            if source["theme_asset"] != asset_id:
                raise SpecError("instance `%s` theme_asset mismatch" % style_id)
            if instance.get("zone") != source.get("zone"):
                raise SpecError("instance `%s` zone mismatch" % style_id)
            if instance.get("proxy_primitive") != source.get("primitive"):
                raise SpecError("instance `%s` proxy_primitive mismatch" % style_id)
            anchor = instance.get("anchor")
            if (not isinstance(anchor, list) or len(anchor) != 3
                    or any(not isinstance(v, (int, float)) for v in anchor)):
                raise SpecError("instance `%s` needs numeric anchor" % style_id)
            if anchor != source.get("position"):
                raise SpecError("instance `%s` anchor must equal style position"
                                % style_id)
            zone = zones[source["zone"]]
            if (abs(float(anchor[0]) - float(zone["center"][0]))
                    > float(zone["half"][0])
                    or abs(float(anchor[2]) - float(zone["center"][1]))
                    > float(zone["half"][1])):
                raise SpecError("instance `%s` anchor outside zone" % style_id)
            scale = instance.get("scale", 1.0)
            rotation = instance.get("rotation_y_deg", 0.0)
            if (not isinstance(scale, (int, float)) or float(scale) <= 0
                    or not isinstance(rotation, (int, float))):
                raise SpecError("instance `%s` needs scale > 0 and numeric rotation"
                                % style_id)
    if set(placed) - covered:
        raise SpecError("style items missing from external_mesh_contract: %s"
                        % sorted(set(placed) - covered))


def load_style_pack(path: str, spec: Dict[str, Any]) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    validate_style_pack(pack, spec)
    validate_external_mesh_contract(
        pack, spec, os.path.dirname(os.path.abspath(path)))
    return pack


def validate_style_pack(pack: Dict[str, Any], spec: Dict[str, Any]) -> None:
    """AI 生成的风格包必须在槽位与预算内，且不得碰核心几何。"""
    if pack.get("schema") != STYLE_SCHEMA:
        raise SpecError("unsupported style pack schema")
    if any(key in pack for key in ("grid", "core_contract", "anchors",
                                   "decoration_zones")):
        raise SpecError("style pack must not override frozen core geometry")
    frame_style = pack.get("frame_style", {})
    if "color" in frame_style:
        _clamp_color(frame_style["color"])

    zones = {zone["id"]: zone for zone in spec["decoration_zones"]["items"]}
    ids: set[str] = set()
    per_zone: Dict[str, int] = {}
    total = 0
    for section in ("decorations", "interactables"):
        items = pack.get(section)
        if not isinstance(items, list):
            raise SpecError("style pack `%s` must be a list" % section)
        for item in items:
            name = item.get("id")
            if not isinstance(name, str) or not name:
                raise SpecError("%s item needs a non-empty `id`" % section)
            if name in ids:
                raise SpecError("duplicate style pack id `%s`" % name)
            ids.add(name)
            if item.get("primitive") not in PRIMITIVES:
                raise SpecError("unknown primitive `%s` in `%s`"
                                % (item.get("primitive"), name))
            if item.get("min_lod") not in LOD_BITS:
                raise SpecError("bad min_lod in `%s`" % name)
            if "external_mesh" in item:
                raise SpecError("`%s` uses external_mesh: consumer not implemented "
                                "yet (schema frozen in external_mesh_contract/v1)"
                                % name)
            position = item.get("position")
            if (not isinstance(position, list) or len(position) != 3
                    or any(not isinstance(v, (int, float)) for v in position)):
                raise SpecError("`%s` needs numeric position [x, y, z]" % name)
            color = item.get("color")
            _clamp_color(DEFAULT_COLOR if color is None else color)
            if section == "interactables" and not item.get("tag"):
                raise SpecError("interactable `%s` needs a `tag`" % name)

            zone_id = item.get("zone")
            if zone_id not in zones:
                raise SpecError("`%s` references unknown zone `%s`" % (name, zone_id))
            zone = zones[zone_id]
            if zone.get("reserved_by"):
                raise SpecError("`%s` uses zone `%s` reserved for %s"
                                % (name, zone_id, zone["reserved_by"]))
            dx = abs(float(position[0]) - float(zone["center"][0]))
            dz = abs(float(position[2]) - float(zone["center"][1]))
            if dx > float(zone["half"][0]) or dz > float(zone["half"][1]):
                raise SpecError("`%s` at %s is outside zone `%s`"
                                % (name, position, zone_id))
            per_zone[zone_id] = per_zone.get(zone_id, 0) + 1
            if per_zone[zone_id] > int(zone["max_items"]):
                raise SpecError("zone `%s` exceeds max_items" % zone_id)
            total += 1
    if total > int(spec["lods"]["HD"]["max_decorations"]):
        raise SpecError("style pack exceeds HD max_decorations budget")


# --------------------------------------------------------------------------
# AI 视觉包：天空盒 / 后处理 / 材质 / Volume
# 与 style-pack 分离 —— 后者只管外围装饰几何，前者只管场景视觉层。
# --------------------------------------------------------------------------

def _visual_number(node: Dict[str, Any], key: str, where: str,
                   minimum: float | None = None,
                   maximum: float | None = None) -> float:
    value = node.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise SpecError("%s.%s must be numeric" % (where, key))
    result = float(value)
    if ((minimum is not None and result < minimum)
            or (maximum is not None and result > maximum)):
        raise SpecError("%s.%s out of range" % (where, key))
    return result


def _visual_sha(value: Any, where: str) -> None:
    _sha256_field(value, where)


def _visual_color(node: Dict[str, Any], key: str, where: str) -> None:
    if key not in node:
        raise SpecError("missing `%s.%s`" % (where, key))
    _clamp_color(node[key])


def _visual_vec4(node: Dict[str, Any], key: str, where: str,
                 minimum: float = -1.0, maximum: float = 8.0) -> None:
    """ColorGrading 的 saturation/contrast/gain 是 HDR 系数，可以大于 1。"""
    if key not in node:
        raise SpecError("missing `%s.%s`" % (where, key))
    value = node[key]
    if (not isinstance(value, list) or len(value) != 4
            or any(not isinstance(v, (int, float)) or isinstance(v, bool)
                   or not minimum <= float(v) <= maximum for v in value)):
        raise SpecError("%s.%s must be 4 numbers in [%g, %g]"
                        % (where, key, minimum, maximum))


def load_visual_pack(path: str, spec: Dict[str, Any]) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    validate_visual_pack(pack, spec)
    return pack


def validate_visual_pack(pack: Dict[str, Any], spec: Dict[str, Any]) -> None:
    """AI 视觉包不得碰冻结几何，且四组参数必须完整、可离线校验。"""
    if pack.get("schema") != VISUAL_SCHEMA:
        raise SpecError("unsupported visual pack schema")
    if any(key in pack for key in ("grid", "core_contract", "anchors",
                                   "decoration_zones", "scene")):
        raise SpecError("visual pack must not override frozen core geometry")
    for section in VISUAL_SECTIONS:
        if not isinstance(pack.get(section), dict):
            raise SpecError("missing `visual.%s`" % section)

    concept = pack["concept"]
    if not concept.get("prompt") or not concept.get("image"):
        raise SpecError("concept.prompt / concept.image are required")
    _visual_sha(concept.get("sha256"), "concept.sha256")

    palette = pack["palette"]
    for key in ("primary", "accent", "glow"):
        _visual_color(palette, key, "palette")

    skybox = pack["skybox"]
    if skybox.get("mode") not in ("procedural", "panorama"):
        raise SpecError("skybox.mode must be procedural or panorama")
    for key in ("top_color", "horizon_color", "bottom_color"):
        _visual_color(skybox, key, "skybox")
    _visual_number(skybox, "exposure", "skybox", -8.0, 8.0)
    _visual_number(skybox, "rotation_deg", "skybox", -360.0, 360.0)
    fog = skybox.get("fog")
    if not isinstance(fog, dict):
        raise SpecError("missing `skybox.fog`")
    _visual_color(fog, "color", "skybox.fog")
    for key in ("enabled", "mode", "density", "linear_start", "linear_end"):
        _visual_number(fog, key, "skybox.fog", 0.0)
    if skybox["mode"] == "panorama":
        if not skybox.get("panorama_image"):
            raise SpecError("panorama mode needs `panorama_image`")
        _visual_sha(skybox.get("panorama_sha256"), "skybox.panorama_sha256")
        size = skybox.get("panorama_size_px")
        if (not isinstance(size, list) or len(size) != 2
                or not all(isinstance(v, int) for v in size)
                or size[0] != 2 * size[1] or size[1] <= 0):
            raise SpecError("panorama_size_px must be a 2:1 [width, height]")

    pp = pack["post_processing"]
    _visual_number(pp, "exposure_value", "post_processing", -8.0, 8.0)
    _visual_number(pp, "tonemap_mode", "post_processing", 0, 8)
    grading = pp.get("color_grading")
    if not isinstance(grading, dict):
        raise SpecError("missing `post_processing.color_grading`")
    for key in ("global_saturation", "global_contrast", "global_gamma",
                "global_gain", "global_offset"):
        _visual_vec4(grading, key, "post_processing.color_grading")
    for key in ("white_balance_temperature", "white_balance_tint",
                "blue_correction", "expand_gamut", "tone_curve_type"):
        _visual_number(grading, key, "post_processing.color_grading")
    bloom = pp.get("bloom")
    if not isinstance(bloom, dict):
        raise SpecError("missing `post_processing.bloom`")
    for key in ("enabled", "threshold", "soft_knee", "intensity", "diffusion",
                "firefly_removal_strength", "clamp"):
        _visual_number(bloom, key, "post_processing.bloom", 0.0)
    _visual_color(bloom, "tint", "post_processing.bloom")
    lut = pp.get("lut")
    if not isinstance(lut, dict) or lut.get("texture") is not None:
        raise SpecError("LUT texture assets are not implemented; keep `lut.texture` null")
    _visual_number(lut, "intensity", "post_processing.lut", 0.0, 1.0)

    materials = pack["materials"]
    if not isinstance(materials, dict) or not materials:
        raise SpecError("visual.materials must be a non-empty map")
    allowed = set(spec["unity"]["color_properties"])
    for name, entry in materials.items():
        if not isinstance(name, str) or not name or not isinstance(entry, dict):
            raise SpecError("invalid visual.materials entry `%s`" % name)
        _visual_color(entry, "color", "materials." + name)
        props = entry.get("properties", {})
        if not isinstance(props, dict) or not set(props) <= allowed:
            raise SpecError("`%s` uses non-whitelisted material properties" % name)

    volume = pack["volume"]
    guid = volume.get("script_guid")
    if (not isinstance(guid, str) or len(guid) != 32
            or any(c not in "0123456789abcdef" for c in guid)):
        raise SpecError("volume.script_guid must be a 32-char hex GUID")
    if not isinstance(volume.get("enabled"), bool):
        raise SpecError("volume.enabled must be a bool")
    _visual_number(volume, "weight", "volume", 0.0, 1.0)
    _visual_number(volume, "blend_distance", "volume", 0.0)
    _visual_number(volume, "priority", "volume", -1000.0, 1000.0)
    _visual_number(volume, "is_global", "volume", 0, 1)
    _visual_number(volume, "script_file_id", "volume", 0)
    if not isinstance(volume.get("override_keys"), list) or not volume["override_keys"]:
        raise SpecError("volume.override_keys must be a non-empty list")

    lighting = pack["lighting"]
    for key in ("directional_color", "ambient_color", "subtractive_shadow_color"):
        _visual_color(lighting, key, "lighting")
    for key in ("directional_intensity", "ambient_intensity"):
        _visual_number(lighting, key, "lighting", 0.0, 8.0)

    contract = pack.get("ai_generation_contract")
    expected = contract.get("expected_fields") if isinstance(contract, dict) else None
    if (not isinstance(expected, list)
            or not {"skybox", "post_processing", "materials", "volume"} <= set(expected)):
        raise SpecError("ai_generation_contract must cover skybox / "
                        "post_processing / materials / volume")


def validate_visual_materials(pack: Dict[str, Any],
                              material_names: Sequence[str]) -> None:
    """视觉包材质名必须与 trunk 已落地的 .mat 集合完全一致（GUID 不变）。"""
    declared = set(pack["materials"])
    actual = set(material_names)
    if declared != actual:
        raise SpecError("visual pack materials != landed Unity materials: "
                        "missing %s / extra %s"
                        % (sorted(actual - declared), sorted(declared - actual)))


def apply_overrides(spec: Dict[str, Any], overrides: Sequence[str]) -> Dict[str, Any]:
    """`--set grid.shape=hex` / `--set sky.radius_m=80` / `--set grid.rows=8`。"""
    out = copy.deepcopy(spec)
    for item in overrides:
        if "=" not in item:
            raise SpecError("bad --set `%s` (want key=value)" % item)
        key, raw = item.split("=", 1)
        node: Any = out
        parts = key.split(".")
        for part in parts[:-1]:
            if not isinstance(node, dict) or part not in node:
                raise SpecError("unknown --set path `%s`" % key)
            node = node[part]
        if parts[-1] not in node:
            raise SpecError("unknown --set path `%s`" % key)
        old = node[parts[-1]]
        try:
            if isinstance(old, bool):
                value: Any = raw.lower() in ("1", "true", "yes")
            elif isinstance(old, int):
                value = int(raw)
            elif isinstance(old, float):
                value = float(raw)
            elif isinstance(old, list):
                value = json.loads(raw)
            else:
                value = raw
        except (ValueError, json.JSONDecodeError) as exc:
            raise SpecError("bad value for `%s`: %s" % (key, exc)) from exc
        node[parts[-1]] = value
    validate_spec(out)
    return out


# --------------------------------------------------------------------------
# 几何容器
# --------------------------------------------------------------------------

@dataclass
class MeshBuilder:
    positions: List[float] = field(default_factory=list)
    normals: List[float] = field(default_factory=list)
    uvs: List[float] = field(default_factory=list)
    indices: List[int] = field(default_factory=list)

    @property
    def vertex_count(self) -> int:
        return len(self.positions) // 3

    def add_tri(self, p0: Vec3, p1: Vec3, p2: Vec3,
                uv0: Vec2 = (0.0, 0.0), uv1: Vec2 = (1.0, 0.0),
                uv2: Vec2 = (0.0, 1.0),
                want_normal: Vec3 | None = None) -> None:
        normal = _cross(_sub(p1, p0), _sub(p2, p0))
        if _dot(normal, normal) < 1.0e-20:
            return
        if want_normal is not None and _dot(normal, want_normal) < 0:
            p1, p2 = p2, p1
            uv1, uv2 = uv2, uv1
            normal = (-normal[0], -normal[1], -normal[2])
        unit = _norm(normal)
        base = self.vertex_count
        for point, uv in ((p0, uv0), (p1, uv1), (p2, uv2)):
            self.positions.extend(point)
            self.normals.extend(unit)
            self.uvs.extend(uv)
        self.indices.extend((base, base + 1, base + 2))

    def add_quad(self, p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3,
                 uvs: Tuple[Vec2, Vec2, Vec2, Vec2] = ((0, 0), (1, 0), (1, 1), (0, 1)),
                 want_normal: Vec3 | None = None) -> None:
        self.add_tri(p0, p1, p2, uvs[0], uvs[1], uvs[2], want_normal)
        self.add_tri(p0, p2, p3, uvs[0], uvs[2], uvs[3], want_normal)

    def transform(self, rotation_y_deg: float, translation: Vec3) -> None:
        for i in range(0, len(self.positions), 3):
            point = self.positions[i:i + 3]
            normal = self.normals[i:i + 3]
            rp = _rotate_y(point, rotation_y_deg)
            rn = _rotate_y(normal, rotation_y_deg)
            self.positions[i:i + 3] = (rp[0] + translation[0],
                                       rp[1] + translation[1],
                                       rp[2] + translation[2])
            self.normals[i:i + 3] = rn

    def bounds(self) -> Tuple[Vec3, Vec3]:
        if not self.positions:
            return ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        lo = [min(self.positions[a::3]) for a in range(3)]
        hi = [max(self.positions[a::3]) for a in range(3)]
        return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))


@dataclass
class Part:
    name: str
    kind: int
    lod_mask: int
    material: str
    tag: str
    color: Tuple[float, float, float, float]
    mesh: MeshBuilder

    @property
    def matrix(self) -> Tuple[float, ...]:
        return (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


# --------------------------------------------------------------------------
# 几何原语（全部由 spec 参数驱动，这是"几何也要变"的落点）
# --------------------------------------------------------------------------

def cell_polygon(shape: str, width: float, height: float,
                 ratio: float, detail: int) -> List[Tuple[float, float]]:
    hw, hh = width / 2.0, height / 2.0
    if shape == "hex":
        # 平顶六边形，适合交错网格（尖角在左右）。
        return [(hw * math.cos(math.pi * i / 3),
                 hh * math.sin(math.pi * i / 3)) for i in range(6)]
    if shape == "rect":
        return [(-hw, -hh), (-hw, hh), (hw, hh), (hw, -hh)]
    cut = min(hw, hh) * max(0.0, min(0.49, ratio))
    corners = [(-hw, -hh), (-hw, hh), (hw, hh), (hw, -hh)]
    # 每个角的 (进入边切点, 离开边切点)。必须沿边界单调前进，
    # 否则多边形自交，侧面外法线会朝内（背面剔除时格子侧壁消失）。
    cuts = [
        ((-hw + cut, -hh), (-hw, -hh + cut)),
        ((-hw, hh - cut), (-hw + cut, hh)),
        ((hw - cut, hh), (hw, hh - cut)),
        ((hw, -hh + cut), (hw - cut, -hh)),
    ]
    polygon: List[Tuple[float, float]] = []
    steps = max(1, int(detail))
    for (cx, cz), (a, b) in zip(corners, cuts):
        for step in range(steps + 1):
            t = step / steps
            it = 1.0 - t
            # 二次贝塞尔，控制点 = 角点；detail 越大倒角越圆滑。
            polygon.append((it * it * a[0] + 2 * it * t * cx + t * t * b[0],
                            it * it * a[1] + 2 * it * t * cz + t * t * b[1]))
    return polygon


def extrude_polygon(polygon: Sequence[Tuple[float, float]],
                    top_y: float, thickness: float) -> MeshBuilder:
    mesh = MeshBuilder()
    up, down = (0.0, 1.0, 0.0), (0.0, -1.0, 0.0)
    xs = [p[0] for p in polygon]
    zs = [p[1] for p in polygon]
    span_x = (max(xs) - min(xs)) or 1.0
    span_z = (max(zs) - min(zs)) or 1.0

    def uv(point: Tuple[float, float]) -> Vec2:
        return ((point[0] - min(xs)) / span_x, (point[1] - min(zs)) / span_z)

    for i in range(1, len(polygon) - 1):
        a, b, c = polygon[0], polygon[i], polygon[i + 1]
        mesh.add_tri((a[0], top_y, a[1]), (b[0], top_y, b[1]),
                     (c[0], top_y, c[1]), uv(a), uv(b), uv(c), up)
        mesh.add_tri((a[0], top_y - thickness, a[1]),
                     (b[0], top_y - thickness, b[1]),
                     (c[0], top_y - thickness, c[1]),
                     uv(a), uv(b), uv(c), down)

    for i, a in enumerate(polygon):
        b = polygon[(i + 1) % len(polygon)]
        edge = (b[0] - a[0], b[1] - a[1])
        # polygon 按 XZ 上的 (+Y 法线) 顺序给出，边方向 (dx, dz) 的外法线是 (-dz, 0, dx)。
        outward = _norm((-edge[1], 0.0, edge[0]))
        a_top = (a[0], top_y, a[1])
        a_bot = (a[0], top_y - thickness, a[1])
        b_bot = (b[0], top_y - thickness, b[1])
        b_top = (b[0], top_y, b[1])
        mesh.add_quad(a_top, a_bot, b_bot, b_top,
                      ((0, 1), (0, 0), (1, 0), (1, 1)), outward)
    return mesh


def primitive_box(size: Sequence[float], top_y: float = 0.0) -> MeshBuilder:
    w, h, d = float(size[0]), float(size[1]), float(size[2])
    poly = [(-w / 2, -d / 2), (-w / 2, d / 2), (w / 2, d / 2), (w / 2, -d / 2)]
    return extrude_polygon(poly, top_y + h / 2, h)


def primitive_cylinder(radius: float, height: float, segments: int,
                       cap: str = "flat", top_y: float = 0.0) -> MeshBuilder:
    mesh = MeshBuilder()
    ring = [(radius * math.cos(2 * math.pi * i / segments),
             radius * math.sin(2 * math.pi * i / segments))
            for i in range(segments)]
    for i in range(segments):
        a = ring[i]
        b = ring[(i + 1) % segments]
        mesh.add_tri((0.0, top_y, 0.0), (a[0], top_y, a[1]),
                     (b[0], top_y, b[1]), (0.5, 0.5), (0, 0), (1, 0), (0, 1, 0))
        mesh.add_tri((0.0, top_y - height, 0.0),
                     (a[0], top_y - height, a[1]),
                     (b[0], top_y - height, b[1]),
                     (0.5, 0.5), (0, 0), (1, 0), (0, -1, 0))
        outward = _norm(((a[0] + b[0]) / 2, 0.0, (a[1] + b[1]) / 2))
        mesh.add_quad((a[0], top_y, a[1]),
                      (a[0], top_y - height, a[1]),
                      (b[0], top_y - height, b[1]),
                      (b[0], top_y, b[1]),
                      ((0, 1), (0, 0), (1, 0), (1, 1)), outward)
    if cap == "pyramid":
        apex = (0.0, top_y + radius * 1.6, 0.0)
        for i in range(segments):
            a, b = ring[i], ring[(i + 1) % segments]
            normal = _norm(((a[0] + b[0]) / 2, radius, (a[1] + b[1]) / 2))
            mesh.add_tri((a[0], top_y, a[1]), apex, (b[0], top_y, b[1]),
                         (0, 0), (0.5, 1), (1, 0), normal)
    return mesh


def primitive_pyramid(base: float, height: float) -> MeshBuilder:
    mesh = MeshBuilder()
    half = base / 2
    poly = [(-half, -half), (-half, half), (half, half), (half, -half)]
    apex = (0.0, height, 0.0)
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        normal = _norm(((a[0] + b[0]) / 2, height * 0.5, (a[1] + b[1]) / 2))
        mesh.add_tri((a[0], 0, a[1]), apex, (b[0], 0, b[1]),
                     (0, 0), (0.5, 1), (1, 0), normal)
    mesh.add_quad((poly[0][0], 0, poly[0][1]), (poly[3][0], 0, poly[3][1]),
                  (poly[2][0], 0, poly[2][1]), (poly[1][0], 0, poly[1][1]),
                  want_normal=(0, -1, 0))
    return mesh


def primitive_ring(inner: float, outer: float, segments: int,
                   y: float = 0.0) -> MeshBuilder:
    mesh = MeshBuilder()
    for i in range(segments):
        a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
        inner_a = (inner * math.cos(a0), y, inner * math.sin(a0))
        outer_a = (outer * math.cos(a0), y, outer * math.sin(a0))
        inner_b = (inner * math.cos(a1), y, inner * math.sin(a1))
        outer_b = (outer * math.cos(a1), y, outer * math.sin(a1))
        mesh.add_quad(inner_a, outer_a, outer_b, inner_b,
                      want_normal=(0, 1, 0))
    return mesh


def primitive_torus(radius: float, tube: float, radial: int,
                    tubular: int) -> MeshBuilder:
    mesh = MeshBuilder()
    for i in range(tubular):
        u0, u1 = 2 * math.pi * i / tubular, 2 * math.pi * (i + 1) / tubular
        for j in range(radial):
            v0, v1 = 2 * math.pi * j / radial, 2 * math.pi * (j + 1) / radial

            def point(u: float, v: float) -> Vec3:
                return ((radius + tube * math.cos(v)) * math.cos(u),
                        tube * math.sin(v),
                        (radius + tube * math.cos(v)) * math.sin(u))

            p00, p10 = point(u0, v0), point(u1, v0)
            p11, p01 = point(u1, v1), point(u0, v1)
            center = point((u0 + u1) / 2, (v0 + v1) / 2)
            normal = _norm((center[0] - radius * math.cos((u0 + u1) / 2),
                            center[1],
                            center[2] - radius * math.sin((u0 + u1) / 2)))
            mesh.add_quad(p00, p10, p11, p01, want_normal=normal)
    return mesh


def primitive_sky_sphere(radius: float, segments: int, rings: int,
                         inward: bool = True) -> MeshBuilder:
    """UV 球。inward=True 法线朝球心（天穹，从内部看）；
    inward=False 法线朝外（风格包里的球体装饰）。
    """
    mesh = MeshBuilder()
    for r in range(rings):
        t0, t1 = math.pi * r / rings, math.pi * (r + 1) / rings
        for s in range(segments):
            p0, p1 = 2 * math.pi * s / segments, 2 * math.pi * (s + 1) / segments

            def point(theta: float, phi: float) -> Vec3:
                return (radius * math.sin(theta) * math.cos(phi),
                        radius * math.cos(theta),
                        radius * math.sin(theta) * math.sin(phi))

            a, b = point(t0, p0), point(t1, p0)
            c, d = point(t1, p1), point(t0, p1)
            center = ((a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2)
            normal = _norm(tuple(-v for v in center) if inward else center)
            mesh.add_quad(a, b, c, d,
                          ((s / segments, r / rings),
                           (s / segments, (r + 1) / rings),
                           ((s + 1) / segments, (r + 1) / rings),
                           ((s + 1) / segments, r / rings)), normal)
    return mesh


PRIMITIVES = {
    "box": primitive_box,
    "cylinder": primitive_cylinder,
    "pyramid": primitive_pyramid,
    "ring": primitive_ring,
    "torus": primitive_torus,
    "sphere": primitive_sky_sphere,
}


def build_primitive(item: Dict[str, Any], place: bool = True) -> MeshBuilder:
    """风格包原语：一律以本地 y=0 为底面，position.y 就是离地高度。

    `place=False` 返回**局部坐标**网格（原点在底面中心、不做旋转）——
    外部 glb 单体资产必须这样交付，锚点与旋转由消费端套用，不能烘进几何。
    """
    params = dict(item.get("params", {}))
    kind = item["primitive"]
    if kind == "box":
        size = params.get("size", [1, 1, 1])
        mesh = primitive_box(size, float(size[1]) / 2)
    elif kind == "cylinder":
        height = params.get("height", 1.0)
        mesh = primitive_cylinder(params.get("radius", 0.5),
                                  height,
                                  max(3, int(params.get("segments", 10))),
                                  params.get("cap", "flat"), top_y=height)
    elif kind == "pyramid":
        mesh = primitive_pyramid(params.get("base", 1.0),
                                 params.get("height", 1.0))
    elif kind == "ring":
        mesh = primitive_ring(params.get("inner", 0.8),
                              params.get("outer", 1.0),
                              max(3, int(params.get("segments", 24))),
                              params.get("y", 0.0))
    elif kind == "torus":
        mesh = primitive_torus(params.get("radius", 0.5),
                               params.get("tube", 0.1),
                               max(3, int(params.get("radial_segments", 8))),
                               max(3, int(params.get("tubular_segments", 16))))
    elif kind == "sphere":
        mesh = primitive_sky_sphere(params.get("radius", 0.5),
                                    max(6, int(params.get("segments", 16))),
                                    max(4, int(params.get("rings", 8))),
                                    inward=False)
    else:
        raise SpecError("unsupported primitive `%s`" % kind)
    if not place:
        return mesh
    position = item["position"]
    mesh.transform(float(item.get("rotation_y_deg", 0.0)),
                   (float(position[0]), float(position[1]), float(position[2])))
    return mesh


# --------------------------------------------------------------------------
# 棋盘装配
# --------------------------------------------------------------------------

def lod_mask_for(min_lod: str) -> int:
    """`min_lod` = 该部件能出现的最低画质档。

    `HD` 表示"只有 HD 才配得上这个昂贵装饰" -> mask 只含 HD bit；
    `Low` 表示"连 Low 档也要保留" -> 四档全含。画质越高，部件越多。
    """
    rank = LOD_RANK[min_lod]
    return sum(bit for name, bit in LOD_BITS.items() if LOD_RANK[name] <= rank)


def cell_center(spec: Dict[str, Any], column: int, row: int) -> Vec3:
    """与 CrossRectangleUtil.MapPosV2ToV3Local / AlignGridUtil 对齐。"""
    grid = spec["grid"]
    stagger = float(grid["stagger_odd_rows_m"]) if grid["layout"] == "cross_rectangle" else 0.0
    offset = grid["center_offset_m"]
    return (column * float(grid["cell_width_m"]) - (row & 1) * stagger + float(offset[0]),
            float(offset[1]),
            row * float(grid["cell_height_m"]) + float(offset[2]))


def logical_bounds(spec: Dict[str, Any]) -> Tuple[float, float, float, float]:
    """逻辑网格占地（只用于算台面尺寸与落点，不生成可见格子）。"""
    grid = spec["grid"]
    centers = [cell_center(spec, c, r)
               for r in range(int(grid["rows"]))
               for c in range(int(grid["columns"]))]
    cw = float(grid["cell_width_m"])
    ch = float(grid["cell_height_m"])
    return (min(p[0] for p in centers) - cw / 2,
            max(p[0] for p in centers) + cw / 2,
            min(p[2] for p in centers) - ch / 2,
            max(p[2] for p in centers) + ch / 2)


def cell_outline_marks(spec: Dict[str, Any], line_width: float) -> MeshBuilder:
    """可选：台面上的浅刻线。是贴着台面的细线，不是 56 个方块。"""
    mesh = MeshBuilder()
    grid = spec["grid"]
    y = float(grid["top_y_m"]) + 0.003
    width = max(0.002, float(line_width))
    poly = cell_polygon("rect", float(grid["cell_width_m"]) - 0.06,
                        float(grid["cell_height_m"]) - 0.06, 0.0, 1)
    for row in range(int(grid["rows"])):
        for column in range(int(grid["columns"])):
            cx, _cy, cz = cell_center(spec, column, row)
            for i, a in enumerate(poly):
                b = poly[(i + 1) % len(poly)]
                ax, az = a[0] + cx, a[1] + cz
                bx, bz = b[0] + cx, b[1] + cz
                dx, dz = bx - ax, bz - az
                length = math.hypot(dx, dz) or 1.0
                nx, nz = -dz / length * width, dx / length * width
                mesh.add_quad((ax, y, az), (bx, y, bz),
                              (bx + nx, y, bz + nz), (ax + nx, y, az + nz),
                              want_normal=(0.0, 1.0, 0.0))
    return mesh


def build_board(spec: Dict[str, Any],
                style_pack: Dict[str, Any] | None = None) -> List[Part]:
    """冻结核心几何 + AI 风格包装饰。

    核心固定：一整块连续台面 / 四段包边 / 功能锚点 / 四档天穹。
    风格可变：style_pack 的装饰物与交互物，数量形态位置全部不固定。
    """
    validate_spec(spec)
    if style_pack is None:
        style_pack = empty_style_pack()
    validate_style_pack(style_pack, spec)

    grid, frame, sky = spec["grid"], spec["frame"], spec["sky"]
    parts: List[Part] = []
    bounds = logical_bounds(spec)
    cx = (bounds[0] + bounds[1]) / 2
    cz = (bounds[2] + bounds[3]) / 2
    margin = float(grid["margin_m"])
    outer_w = bounds[1] - bounds[0] + 2 * margin
    outer_d = bounds[3] - bounds[2] + 2 * margin

    # 1) 核心：一整块连续台面（现网所有皮肤都是这样，不是棋盘格方块）。
    polygon = cell_polygon(grid["shape"], outer_w, outer_d,
                           float(grid["chamfer_ratio"]), int(grid["detail"]))
    slab = extrude_polygon(polygon, float(grid["top_y_m"]),
                           float(grid["thickness_m"]))
    slab.transform(0.0, (cx, 0.0, cz))
    parts.append(Part("core_slab", KIND_GRID, ALL_LODS, "core_slab", "",
                      _clamp_color(grid["slab_color"]), slab))

    # 2) 可选浅刻线：默认 none，纹样应交给材质/贴图。
    if grid["cell_marks"] == "engraved":
        marks = cell_outline_marks(spec, float(grid.get("cell_mark_width_m", 0.018)))
        parts.append(Part("cell_marks", KIND_GRID, ALL_LODS, "cell_marks", "",
                          _clamp_color(grid["cell_mark_color"]), marks))

    # 3) 包边四段（基本形固定，颜色可由风格包覆盖）。
    fw = float(frame["width_m"])
    fh = float(frame["height_m"])
    frame_top = float(grid["top_y_m"]) + fh / 2
    frame_color = _clamp_color(style_pack.get("frame_style", {}).get(
        "color", frame["color"]))

    def rect_poly(width: float, depth: float) -> List[Tuple[float, float]]:
        return [(-width / 2, -depth / 2), (-width / 2, depth / 2),
                (width / 2, depth / 2), (width / 2, -depth / 2)]

    for side, (ox, oz, sx, sz) in enumerate((
            (0.0, -outer_d / 2 - fw / 2, outer_w + 2 * fw, fw),
            (0.0, outer_d / 2 + fw / 2, outer_w + 2 * fw, fw),
            (-outer_w / 2 - fw / 2, 0.0, fw, outer_d),
            (outer_w / 2 + fw / 2, 0.0, fw, outer_d))):
        mesh = extrude_polygon(rect_poly(sx, sz), frame_top, fh)
        mesh.transform(0.0, (cx + ox, 0.0, cz + oz))
        parts.append(Part("frame_%d" % side, KIND_FRAME, ALL_LODS,
                          "frame", "", frame_color, mesh))

    # 4) 两个棋手占位：左下角 / 右上角对角，跨皮肤位置稳定。
    #    圆形代理只冻结位置与占地，具体桌椅由概念图输出。
    for anchor in spec["anchors"]["items"]:
        height = float(anchor["height_m"])
        mesh = primitive_cylinder(float(anchor["radius_m"]), height, 24,
                                  "flat", top_y=height)
        p = anchor["position"]
        mesh.transform(0.0, (float(p[0]), float(p[1]), float(p[2])))
        parts.append(Part("anchor_" + anchor["id"], KIND_FRAME, ALL_LODS,
                          "anchor_" + anchor["id"], "",
                          _clamp_color(anchor["color"]), mesh))

    # 5) 四档天穹：LOD 切换是真实几何变化。
    for name in LOD_BITS:
        segments = max(6, int(spec["lods"][name]["sky_segments"]))
        rings = max(4, segments // 2)
        mesh = primitive_sky_sphere(float(sky["radius_m"]), segments, rings)
        parts.append(Part("sky@%s" % name, KIND_SKY, LOD_BITS[name],
                          "sky_gradient", "", _clamp_color(sky["horizon_color"]), mesh))

    # 6) 风格化部分：数量 / 形态 / 位置 / 颜色全部由 AI 风格包决定。
    for section, kind, prefix in (("decorations", KIND_DECORATION, "decoration_"),
                                  ("interactables", KIND_INTERACTABLE, "interactable_")):
        for item in style_pack[section]:
            mesh = build_primitive(item)
            parts.append(Part(
                name=prefix + item["id"], kind=kind,
                lod_mask=lod_mask_for(item["min_lod"]),
                material="zboard_" + item["id"], tag=item.get("tag", ""),
                color=_clamp_color(DEFAULT_COLOR if item.get("color") is None
                                   else item["color"]),
                mesh=mesh,
            ))

    return parts


# --------------------------------------------------------------------------
# ZBRD 编解码
# --------------------------------------------------------------------------

def decode_zbrd(data: bytes) -> List[Part]:
    if len(data) < 16:
        raise ZBoardError("ZBRD too short")
    magic, version, count, _flags = struct.unpack_from("<4I", data, 0)
    if magic != MAGIC:
        raise ZBoardError("bad ZBRD magic")
    if version != VERSION:
        raise ZBoardError("unsupported ZBRD version %d" % version)
    offset = 16
    parts: List[Part] = []

    def read_string() -> str:
        nonlocal offset
        if offset + 4 > len(data):
            raise ZBoardError("truncated string length")
        (length,) = struct.unpack_from("<I", data, offset)
        offset += 4
        if offset + length > len(data) or length > 1 << 20:
            raise ZBoardError("truncated / oversized string")
        raw = data[offset:offset + length]
        offset += length
        return raw.decode("utf-8")

    def read_floats(n: int) -> List[float]:
        nonlocal offset
        size = n * 4
        if offset + size > len(data):
            raise ZBoardError("truncated float array")
        values = list(struct.unpack_from("<%df" % n, data, offset))
        offset += size
        return values

    for _ in range(count):
        name = read_string()
        if offset + 4 > len(data):
            raise ZBoardError("truncated kind/lod")
        kind, lod_mask, _pad0, _pad1 = struct.unpack_from("<4B", data, offset)
        offset += 4
        material, tag = read_string(), read_string()
        if offset + 8 > len(data):
            raise ZBoardError("truncated counts")
        vc, ic = struct.unpack_from("<2I", data, offset)
        offset += 8
        matrix = read_floats(16)
        rgba = read_floats(4)
        color = (rgba[0], rgba[1], rgba[2], rgba[3])
        positions = read_floats(3 * vc)
        normals = read_floats(3 * vc)
        uvs = read_floats(2 * vc)
        if offset + 4 * ic > len(data):
            raise ZBoardError("truncated indices")
        indices = list(struct.unpack_from("<%dI" % ic, data, offset))
        offset += 4 * ic
        mesh = MeshBuilder(positions, normals, uvs, indices)
        parts.append(Part(name, kind, lod_mask, material, tag, color, mesh))
        del matrix
    if offset != len(data):
        raise ZBoardError("trailing bytes after last part")
    return parts


def encode(parts: Sequence[Part]) -> bytes:
    out = bytearray(struct.pack("<4I", MAGIC, VERSION, len(parts), 0))
    for part in parts:
        mesh = part.mesh
        vc, ic = mesh.vertex_count, len(mesh.indices)
        # v1 早期实现把每个三角面去索引成「三角形汤」(ic == vc)，但格式本身把
        # vertex_count / index_count 分开存，Web(setIndex) 与 Unity(SetTriangles)
        # 都按 index_count 读索引 —— 共享顶点的索引网格是合法的。放宽这条自加
        # 校验后，glb 缓存可保留 glb 自带的顶点表，不再膨胀成 3*面数 个顶点。
        if ic % 3 or any(not (0 <= i < vc) for i in mesh.indices):
            raise ZBoardError("part `%s` has invalid indices" % part.name)
        if len(mesh.normals) != 3 * vc or len(mesh.uvs) != 2 * vc:
            raise ZBoardError("part `%s` has mismatched attribute counts" % part.name)
        name = part.name.encode("utf-8")
        material = part.material.encode("utf-8")
        tag = part.tag.encode("utf-8")
        out.extend(struct.pack("<I", len(name)) + name)
        out.extend(struct.pack("<4B", part.kind, part.lod_mask, 0, 0))
        out.extend(struct.pack("<I", len(material)) + material)
        out.extend(struct.pack("<I", len(tag)) + tag)
        out.extend(struct.pack("<2I", vc, ic))
        out.extend(struct.pack("<16f", *part.matrix))
        out.extend(struct.pack("<4f", *part.color))
        out.extend(struct.pack("<%df" % (3 * vc), *mesh.positions))
        out.extend(struct.pack("<%df" % (3 * vc), *mesh.normals))
        out.extend(struct.pack("<%df" % (2 * vc), *mesh.uvs))
        out.extend(struct.pack("<%dI" % ic, *mesh.indices))
    return bytes(out)


CORE_PREFIXES = ("core_slab", "frame_", "anchor_", "sky@")
VARIABLE_PREFIXES = ("cell_marks", "decoration_", "interactable_")


def is_core_part(name: str) -> bool:
    return name.startswith(CORE_PREFIXES)


def is_variable_part(name: str) -> bool:
    return name.startswith(VARIABLE_PREFIXES)


def manifest(spec: Dict[str, Any], parts: Sequence[Part], data: bytes,
             style_pack: Dict[str, Any] | None = None,
             visual_pack: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """清单同时承载冻结核心、可变几何风格与 AI 场景视觉层。"""
    if style_pack is None:
        style_pack = empty_style_pack()
    per_lod: Dict[str, Any] = {}
    for name, bit in LOD_BITS.items():
        selected = [p for p in parts if p.lod_mask & bit]
        per_lod[name] = {
            "parts": len(selected),
            "vertices": sum(p.mesh.vertex_count for p in selected),
            "star_scale": spec["lods"][name].get("star_scale", 1.0),
            "decorations": len([p for p in selected
                                if p.kind in (KIND_DECORATION, KIND_INTERACTABLE)]),
            "max_decorations": int(spec["lods"][name]["max_decorations"]),
        }
    kinds: Dict[str, int] = {}
    for part in parts:
        kinds[KIND_NAMES[part.kind]] = kinds.get(KIND_NAMES[part.kind], 0) + 1
    hints = {item["id"]: item.get("hint", "")
             for item in style_pack.get("interactables", [])}
    bounds = logical_bounds(spec)
    grid = spec["grid"]
    info = {
        "schema": "zengine.chessboard.manifest/v1",
        "name": spec["name"],
        "skin_hint": spec["skin_hint"],
        "target_branch": spec["target"]["branch"],
        # Unity 交付物是「一个高配 .unity 场景」，不是四档 prefab。
        "scene": spec["scene"],
        "delivered_lod": "HD",
        "scene_parts": len([p for p in parts if p.lod_mask & LOD_BITS["HD"]]),
        "core_contract": spec["core_contract"],
        "grid": {
            "layout": grid["layout"],
            "columns": grid["columns"],
            "rows": grid["rows"],
            "cell_width_m": grid["cell_width_m"],
            "cell_height_m": grid["cell_height_m"],
            "shape": grid["shape"],
            "visual_kind": spec["core_contract"]["visual_kind"],
            "logical_grid_is_visual": spec["core_contract"]["logical_grid_is_visual"],
            "cell_marks": grid["cell_marks"],
            "logical_bounds_xz": list(bounds),
        },
        "frame": spec["frame"],
        "anchors": [{"id": a["id"], "position": a["position"]}
                    for a in spec["anchors"]["items"]],
        "decoration_zones": spec["decoration_zones"]["items"],
        "style_pack": {
            "schema": style_pack["schema"],
            "name": style_pack.get("name", ""),
            "generated_by": style_pack.get("generated_by", ""),
            "status": style_pack.get("status", ""),
            "decorations": len(style_pack.get("decorations", [])),
            "interactables": len(style_pack.get("interactables", [])),
        },
        "core_parts": [p.name for p in parts if is_core_part(p.name)],
        "variable_parts": [p.name for p in parts if is_variable_part(p.name)],
        "parts": len(parts),
        "kinds": kinds,
        "interactables": [{"id": p.name, "tag": p.tag,
                           "hint": hints.get(p.name[len("interactable_"):], "")}
                          for p in parts if p.kind == KIND_INTERACTABLE],
        # Unity 生成器用 JsonUtility 直接读这些扁平字段，不用再解析 spec。
        "root_prefix": "P_ART_OS_" + spec["skin_hint"],
        "unity": spec["unity"],
        "lighting": spec["lighting"],
        "sky": {key: spec["sky"][key] for key in
                ("mode", "top_color", "horizon_color", "bottom_color", "radius_m")},
        "lods": per_lod,
        "zbrd_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    contract = style_pack.get("external_mesh_contract")
    if contract is not None:
        statuses: Dict[str, int] = {}
        for entry in contract["items"]:
            statuses[entry["status"]] = statuses.get(entry["status"], 0) + 1
        info["external_mesh"] = {
            "schema": contract["schema"],
            "asset_format": contract["asset_format"],
            "frozen": True,
            "assets": len(contract["items"]),
            "instances": sum(len(entry["instances"]) for entry in contract["items"]),
            "consumed": sum(len(entry["instances"]) for entry in contract["items"]
                            if entry["status"] == "asset_ready"),
            "statuses": statuses,
            "consumer": "glb + sha256 + anchor (unity / blender / web)",
        }
    if visual_pack is not None:
        skybox = visual_pack["skybox"]
        pp = visual_pack["post_processing"]
        info["visual"] = {
            "schema": visual_pack["schema"],
            "name": visual_pack.get("name", ""),
            "generated_by": visual_pack.get("generated_by", ""),
            "status": visual_pack.get("status", ""),
            "concept_image": visual_pack["concept"]["image"],
            "concept_sha256": visual_pack["concept"]["sha256"],
            "skybox_mode": skybox["mode"],
            "panorama_image": skybox.get("panorama_image"),
            "panorama_sha256": skybox.get("panorama_sha256"),
            "sky_colors": {key: skybox[key] for key in
                           ("top_color", "horizon_color", "bottom_color")},
            "fog": skybox["fog"],
            "post_processing": {
                "exposure_value": pp["exposure_value"],
                "tonemap_mode": pp["tonemap_mode"],
                "bloom": pp["bloom"],
                "color_grading": pp["color_grading"],
            },
            "materials": sorted(visual_pack["materials"]),
            "volume": {
                "enabled": visual_pack["volume"]["enabled"],
                "script_guid": visual_pack["volume"]["script_guid"],
                "weight": visual_pack["volume"]["weight"],
            },
            "lighting": visual_pack["lighting"],
        }
    return info


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: Sequence[str] | None = None) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="spec -> ZBRD (three.js / Unity 共享几何)")
    parser.add_argument("--spec", default=os.path.join(here, "board-spec.json"))
    parser.add_argument("--style", default=os.path.join(here, "style-pack.json"),
                        help="AI/人工可替换的风格包；核心几何不受它影响")
    parser.add_argument("--no-style", action="store_true",
                        help="只生成冻结核心几何，不加载任何风格包")
    parser.add_argument("--visual", default=os.path.join(here, "visual-pack.json"),
                        help="AI 视觉包（天空盒 / 后处理 / 材质 / Volume）")
    parser.add_argument("--no-visual", action="store_true",
                        help="不加载视觉包，manifest 里不带 visual 段")
    parser.add_argument("--out", default=os.path.join(here, "build", "board.zbrd"))
    parser.add_argument("--manifest-out", default="")
    parser.add_argument("--set", action="append", default=[], dest="overrides")
    parser.add_argument("--variants", action="store_true",
                        help="额外生成 grid.shape_variants 里的每个几何变体")
    parser.add_argument("--check", action="store_true", help="只校验 spec + 风格包")
    args = parser.parse_args(argv)

    try:
        spec = load_spec(args.spec)
        if args.overrides:
            spec = apply_overrides(spec, args.overrides)
        style_pack = empty_style_pack() if args.no_style else load_style_pack(args.style, spec)
        if args.no_style:
            validate_style_pack(style_pack, spec)
        visual_pack = None if args.no_visual else load_visual_pack(args.visual, spec)
        if args.check:
            print("SPEC OK: %s" % spec["name"])
            print("STYLE OK: %s (%d decorations / %d interactables)"
                  % (style_pack.get("name", ""), len(style_pack["decorations"]),
                     len(style_pack["interactables"])))
            print("VISUAL OK: %s (%s skybox / %d materials)"
                  % (visual_pack["name"] if visual_pack else "(none)",
                     visual_pack["skybox"]["mode"] if visual_pack else "-",
                     len(visual_pack["materials"]) if visual_pack else 0))
            return 0

        parts = build_board(spec, style_pack)
        data = encode(parts)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "wb") as handle:
            handle.write(data)
        info = manifest(spec, parts, data, style_pack, visual_pack)
        manifest_path = args.manifest_out or os.path.join(
            os.path.dirname(os.path.abspath(args.out)), "board.manifest.json")
        with open(manifest_path, "w", encoding="utf-8") as handle:
            json.dump(info, handle, ensure_ascii=False, indent=2)
        print("WROTE %s (%d bytes, %d parts = %d core + %d variable)"
              % (args.out, len(data), len(parts),
                 len(info["core_parts"]), len(info["variable_parts"])))
        print("MANIFEST %s sha256=%s" % (manifest_path, info["sha256"][:16]))

        if args.variants:
            outdir = os.path.join(os.path.dirname(os.path.abspath(args.out)), "variants")
            os.makedirs(outdir, exist_ok=True)
            listing = []
            for shape in spec["grid"].get("shape_variants", []):
                variant = apply_overrides(spec, ["grid.shape=" + shape])
                variant_parts = build_board(variant, style_pack)
                variant_data = encode(variant_parts)
                filename = "board_%s.zbrd" % shape
                with open(os.path.join(outdir, filename), "wb") as handle:
                    handle.write(variant_data)
                info_v = manifest(variant, variant_parts, variant_data, style_pack)
                listing.append({"shape": shape, "file": "variants/" + filename,
                                "sha256": info_v["sha256"],
                                "vertices": info_v["lods"]["HD"]["vertices"]})
                print("VARIANT %-8s -> %s (%d bytes)"
                      % (shape, filename, len(variant_data)))
            index_path = os.path.join(
                os.path.dirname(os.path.abspath(args.out)), "variants.json")
            with open(index_path, "w", encoding="utf-8") as handle:
                json.dump({"default": os.path.basename(args.out),
                           "variants": listing}, handle, indent=2)
            print("VARIANT INDEX %s" % index_path)
        # 只在「正式发布构建」时刷新 Blender 交付物：临时目录里的 --no-style /
        # --set / 变体测试不能覆盖 build/blender/，否则交付物会被污染。
        official_build = (
            args.variants and not args.no_style and not args.overrides
            and os.path.abspath(args.out)
            == os.path.abspath(os.path.join(here, "build", "board.zbrd")))
        if official_build:
            # 先落锚点清单，Blender 导出要把它嵌进 bpy 脚本。
            # 用动态导入避免 zboard <-> external_mesh 的静态循环。
            external_mesh = importlib.import_module("external_mesh")
            info_x = external_mesh.write_manifest(style_pack, spec)
            print("EXTERNAL %s (%d assets / %d placements consumed)"
                  % (external_mesh.MANIFEST_NAME, info_x["summary"]["assets"],
                     info_x["summary"]["consumed"]))
        if official_build and visual_pack is not None:
            blender_export = importlib.import_module("blender_export")
            report = blender_export.export_all(spec, parts, visual_pack)
            print("BLENDER %s (%d HD parts / %d materials / %d external)"
                  % (blender_export.BLEND_NAME, report["hd_parts"],
                     report["materials"], report["external_placements"]))
        return 0
    except (SpecError, ZBoardError, ValueError, OSError,
            json.JSONDecodeError) as exc:
        print("ERROR: %s" % exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
