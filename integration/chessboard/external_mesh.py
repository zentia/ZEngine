"""glb + sha256 + 锚点：外部单体装饰资产的三端共享消费层。

设计
----
混元 3D（或其他来源）的 glb **不烘进** ZBRD 共享几何：ZBRD 里仍是 primitive
代理几何与 `zboard_<id>` 材质名。本模块是三个消费端唯一的锚点真源：

* 读 `style-pack.json` 的 `external_mesh_contract/v1`，逐字节校验 glb sha256；
* 生成 `build/external_mesh.manifest.json` —— 每个实例的 anchor / rotation /
  scale / pivot_offset / world_bounds，Unity、Blender、Web 都只消费这一份；
* 为 Unity 生成一份 **ZBRD v1 单部件网格缓存**（`<asset_id>.zbrd`，同样
  sha256 锁定）。Unity 没有 glTF 运行库，缓存让它用已有的 ZBRD 读取路径建
  Mesh；glb 仍是三端交换格式，缓存只是派生产物，两个哈希都校验。

坐标契约
--------
glb 是 glTF 标准 **Y-up 右手系**，本地原点应落在底面中心。真实资产未必遵守，
所以 manifest 里给每个资产算 `pivot_offset`：

    world = anchor + rotY(rotation_y_deg) * (scale * (local + pivot_offset))

`pivot_offset` 把资产 bbox 的 XZ 中心与底面拉到原点。代理资产它恒为 0。
Blender 侧由 glTF importer 转 Z-up，脚本里位置换算是 ``(x, -z, y)``、绕 Z 转
``+rotation_y_deg``；不要在脚本外再转一次。

用法
----
    python external_mesh.py --check            # 校验契约 + glb/cache/manifest
    python external_mesh.py                    # 只刷新 manifest
    python external_mesh.py --write-proxies    # 用 ZBRD 代理原语重出 4 个 glb
    python external_mesh.py --refresh-caches  # 缓存格式升级：按 glb 重派生 ZBRD，不动 glb
    python external_mesh.py --extract-textures # 抽出 glb 内嵌 baseColor PNG 供三端共用
    python external_mesh.py --import-glb star_pillar \
        --source assets/external_meshes/star_pillar.hunyuan.glb --height 2.444

`--import-glb` 是 `--write-proxies` 的逆操作：把混元 3D（或手工）产出的 glb
接管成正式资产——**glb 字节一个都不改**（`mesh_sha256` 与来源文件一致），
尺寸/原点差异全部写进契约的 `pivot_offset` 与 `instances[].scale`，
再离线派生 Unity 用的 ZBRD 缓存。不要用外部工具预处理 glb，否则 sha256 失去
「与上游产物逐字节一致」的意义。
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import math
import os
import struct
from typing import Any, Dict, List, Sequence, Tuple

import zboard  # pyright: ignore[reportImplicitRelativeImport]

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
STYLE_PATH = os.path.join(ROOT, "style-pack.json")
SPEC_PATH = os.path.join(ROOT, "board-spec.json")
VISUAL_PATH = os.path.join(ROOT, "visual-pack.json")

PROXY_DIR = "assets/external_meshes"
MANIFEST_NAME = "external_mesh.manifest.json"
MANIFEST_SCHEMA = "zengine.chessboard.external-mesh-manifest/v1"
PIVOT_EPSILON = 1.0e-4

COMPONENT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
COMPONENT_FORMAT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_DIM = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# --------------------------------------------------------------------------
# 小工具
# --------------------------------------------------------------------------

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def _glb_module() -> Any:
    """动态导入 blender_export，避免 zboard <-> external_mesh 静态循环。"""
    return importlib.import_module("blender_export")


def _split_glb(data: bytes) -> Tuple[Dict[str, Any], bytes]:
    """返回 (JSON 文档, BIN chunk 字节)。结构合法性由 parse_glb 保证。"""
    doc = _glb_module().parse_glb(data)
    json_len = struct.unpack_from("<I", data, 12)[0]
    bin_at = 20 + json_len
    bin_len = struct.unpack_from("<I", data, bin_at)[0]
    return doc, data[bin_at + 8:bin_at + 8 + bin_len]


def _read_accessor(doc: Dict[str, Any], blob: bytes,
                   index: int) -> List[float]:
    accessor = doc["accessors"][index]
    view = doc["bufferViews"][accessor["bufferView"]]
    component = accessor["componentType"]
    dim = TYPE_DIM[accessor["type"]]
    size = COMPONENT_SIZE[component]
    stride = view.get("byteStride") or size * dim
    base = view["byteOffset"] + accessor.get("byteOffset", 0)
    count = accessor["count"]
    fmt = "<%d%s" % (dim, COMPONENT_FORMAT[component])
    values: List[float] = []
    for i in range(count):
        offset = base + i * stride
        if offset + size * dim > len(blob):
            raise ValueError("accessor %d exceeds the BIN chunk" % index)
        values.extend(struct.unpack_from(fmt, blob, offset))
    return values


# --------------------------------------------------------------------------
# 4x4 列主序矩阵（glTF 约定）
# --------------------------------------------------------------------------

def _identity() -> List[float]:
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def _multiply(a: Sequence[float], b: Sequence[float]) -> List[float]:
    out = [0.0] * 16
    for column in range(4):
        for row in range(4):
            out[column * 4 + row] = sum(a[k * 4 + row] * b[column * 4 + k]
                                        for k in range(4))
    return out


def _node_matrix(node: Dict[str, Any]) -> List[float]:
    if "matrix" in node:
        return [float(v) for v in node["matrix"]]
    tx, ty, tz = (float(v) for v in node.get("translation", [0.0, 0.0, 0.0]))
    qx, qy, qz, qw = (float(v) for v in node.get("rotation", [0.0, 0.0, 0.0, 1.0]))
    sx, sy, sz = (float(v) for v in node.get("scale", [1.0, 1.0, 1.0]))
    xx, yy, zz = qx * qx, qy * qy, qz * qz
    xy, xz, yz = qx * qy, qx * qz, qy * qz
    wx, wy, wz = qw * qx, qw * qy, qw * qz
    return [
        (1 - 2 * (yy + zz)) * sx, 2 * (xy + wz) * sx, 2 * (xz - wy) * sx, 0.0,
        2 * (xy - wz) * sy, (1 - 2 * (xx + zz)) * sy, 2 * (yz + wx) * sy, 0.0,
        2 * (xz + wy) * sz, 2 * (yz - wx) * sz, (1 - 2 * (xx + yy)) * sz, 0.0,
        tx, ty, tz, 1.0,
    ]


def _apply_point(m: Sequence[float], point: Sequence[float]) -> Tuple[float, float, float]:
    x, y, z = point
    return (m[0] * x + m[4] * y + m[8] * z + m[12],
            m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14])


def _normal_matrix(m: Sequence[float]) -> List[float]:
    """3x3 逆转置（列主序，9 元素）。非均匀缩放下法线才不会被拉歪。"""
    a, b, c = m[0], m[4], m[8]
    d, e, f = m[1], m[5], m[9]
    g, h, i = m[2], m[6], m[10]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1.0e-12:
        return [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    inv = 1.0 / det
    return [
        (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
        (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
        (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv,
    ]


def _apply_normal(nm: Sequence[float],
                  normal: Sequence[float]) -> Tuple[float, float, float]:
    x, y, z = normal
    nx = nm[0] * x + nm[3] * y + nm[6] * z
    ny = nm[1] * x + nm[4] * y + nm[7] * z
    nz = nm[2] * x + nm[5] * y + nm[8] * z
    length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
    return (nx / length, ny / length, nz / length)


# --------------------------------------------------------------------------
# glb 写入（只写单网格，供代理资产与测试用）
# --------------------------------------------------------------------------

def write_mesh_glb(name: str, material: str, color: Sequence[float],
                   mesh: zboard.MeshBuilder, extras: Dict[str, Any]) -> bytes:
    mod = _glb_module()
    binary = bytearray()
    views: List[Dict[str, Any]] = []
    accessors: List[Dict[str, Any]] = []

    def add_accessor(values: Sequence[float], component: int, kind: str,
                     target: int, bounds: bool = False) -> int:
        fmt = "f" if component == mod.FLOAT else "I"
        raw = struct.pack("<%d%s" % (len(values), fmt), *values)
        views.append({"buffer": 0, "byteOffset": len(binary),
                      "byteLength": len(raw), "target": target})
        binary.extend(raw)
        dim = TYPE_DIM[kind]
        accessor: Dict[str, Any] = {
            "bufferView": len(views) - 1,
            "componentType": component,
            "count": len(values) // dim,
            "type": kind,
        }
        if bounds:
            accessor["min"] = [min(values[i::dim]) for i in range(dim)]
            accessor["max"] = [max(values[i::dim]) for i in range(dim)]
        accessors.append(accessor)
        return len(accessors) - 1

    position = add_accessor(mesh.positions, mod.FLOAT, "VEC3",
                            mod.ARRAY_BUFFER, bounds=True)
    normal = add_accessor(mesh.normals, mod.FLOAT, "VEC3",
                          mod.ARRAY_BUFFER)
    uv = add_accessor(mesh.uvs, mod.FLOAT, "VEC2",
                      mod.ARRAY_BUFFER)
    index = add_accessor(mesh.indices, mod.UINT, "SCALAR",
                         mod.ELEMENT_ARRAY_BUFFER)
    doc = {
        "asset": {"version": "2.0",
                  "generator": "ZEngine zboard.py / external_mesh.py"},
        "scene": 0,
        "scenes": [{"name": "ZEngine_External", "nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0,
                   "extras": {"zengine_asset": name,
                              "zengine_material": material}}],
        "meshes": [{"name": name, "primitives": [{
            "attributes": {"POSITION": position, "NORMAL": normal,
                           "TEXCOORD_0": uv},
            "indices": index, "material": 0, "mode": 4,
        }]}],
        "materials": [{
            "name": material,
            "pbrMetallicRoughness": {
                "baseColorFactor": mod.linear_color(color),
                "metallicFactor": 0.0,
                "roughnessFactor": 0.85,
            },
            "doubleSided": True,
            "extras": {"zengine_material": material},
        }],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
        "extras": dict(extras, schema="zengine.chessboard.external-glb/v1",
                       local_origin="base center, glTF Y-up right-handed"),
    }
    payload = json.dumps(doc, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
    json_chunk = payload + b" " * (-len(payload) % 4)
    bin_chunk = bytes(binary) + b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    return b"".join((
        struct.pack("<3I", mod.GLB_MAGIC, 2, total),
        struct.pack("<2I", len(json_chunk), mod.CHUNK_JSON), json_chunk,
        struct.pack("<2I", len(bin_chunk), mod.CHUNK_BIN), bin_chunk,
    ))


# --------------------------------------------------------------------------
# glb -> ZBRD 单部件（Unity 网格缓存）
# --------------------------------------------------------------------------

def glb_triangles(data: bytes) -> Tuple[List[float], List[float], List[float], int]:
    """展开所有三角面（去索引），返回 (positions, normals, uvs, triangle_count)。

    ZBRD v1 要求「三角形汤」：indices 必须是 0..n-1，所以这里直接 de-index。
    """
    doc, blob = _split_glb(data)
    positions: List[float] = []
    normals: List[float] = []
    uvs: List[float] = []
    triangles = 0

    def walk(node_id: int, parent: Sequence[float]) -> None:
        nonlocal triangles
        node = doc["nodes"][node_id]
        matrix = _multiply(parent, _node_matrix(node))
        if "mesh" in node:
            norm_matrix = _normal_matrix(matrix)
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                if int(primitive.get("mode", 4)) != 4:
                    raise ValueError("only TRIANGLES (mode 4) is supported")
                attributes = primitive["attributes"]
                if "POSITION" not in attributes:
                    raise ValueError("primitive has no POSITION")
                pos = _read_accessor(doc, blob, attributes["POSITION"])
                nrm = (_read_accessor(doc, blob, attributes["NORMAL"])
                       if "NORMAL" in attributes else [])
                tex = (_read_accessor(doc, blob, attributes["TEXCOORD_0"])
                       if "TEXCOORD_0" in attributes else [])
                if "indices" in primitive:
                    idx = [int(v) for v in _read_accessor(doc, blob,
                                                          primitive["indices"])]
                else:
                    idx = list(range(len(pos) // 3))
                if len(idx) % 3:
                    raise ValueError("index count is not a multiple of 3")
                for t in range(0, len(idx), 3):
                    corners = idx[t:t + 3]
                    points = [_apply_point(matrix, pos[i * 3:i * 3 + 3])
                              for i in corners]
                    if nrm:
                        corner_normals = [_apply_normal(norm_matrix,
                                                        nrm[i * 3:i * 3 + 3])
                                          for i in corners]
                    else:
                        edge1 = (points[1][0] - points[0][0],
                                 points[1][1] - points[0][1],
                                 points[1][2] - points[0][2])
                        edge2 = (points[2][0] - points[0][0],
                                 points[2][1] - points[0][1],
                                 points[2][2] - points[0][2])
                        nx = edge1[1] * edge2[2] - edge1[2] * edge2[1]
                        ny = edge1[2] * edge2[0] - edge1[0] * edge2[2]
                        nz = edge1[0] * edge2[1] - edge1[1] * edge2[0]
                        length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
                        corner_normals = [(nx / length, ny / length,
                                           nz / length)] * 3
                    for corner, point, normal in zip(corners, points,
                                                     corner_normals):
                        positions.extend(point)
                        normals.extend(normal)
                        if tex:
                            uvs.extend((tex[corner * 2], tex[corner * 2 + 1]))
                        else:
                            uvs.extend((0.0, 0.0))
                    triangles += 1
        for child in node.get("children", []):
            walk(int(child), matrix)

    scene = doc.get("scenes", [{}])[doc.get("scene", 0)]
    for root in scene.get("nodes", []):
        walk(int(root), _identity())
    return positions, normals, uvs, triangles


def glb_bounds(data: bytes) -> Tuple[Tuple[float, float, float],
                                     Tuple[float, float, float]]:
    positions, _normals, _uvs, triangles = glb_triangles(data)
    if not triangles:
        raise ValueError("glb has no triangles")
    lo = [min(positions[a::3]) for a in range(3)]
    hi = [max(positions[a::3]) for a in range(3)]
    return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))


def glb_indexed(data: bytes) -> Tuple[List[float], List[float], List[float],
                                      List[int]]:
    """保留 glb 的索引网格，返回 (positions, normals, uvs, indices)。

    节点变换（混元产物的 +90°X）烘进顶点 / 法线，与 glb_triangles 同口径。
    glb 自带 POSITION/NORMAL/TEXCOORD_0 与 indices，同一个顶点可被多个三角面
    引用；直接复用顶点表即可，不必展开成 3*面数 个顶点的「三角形汤」。
    """
    doc, blob = _split_glb(data)
    positions: List[float] = []
    normals: List[float] = []
    uvs: List[float] = []
    indices: List[int] = []

    def walk(node_id: int, parent: Sequence[float]) -> None:
        node = doc["nodes"][node_id]
        matrix = _multiply(parent, _node_matrix(node))
        if "mesh" in node:
            norm_matrix = _normal_matrix(matrix)
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                if int(primitive.get("mode", 4)) != 4:
                    raise ValueError("only TRIANGLES (mode 4) is supported")
                attributes = primitive["attributes"]
                if "POSITION" not in attributes:
                    raise ValueError("primitive has no POSITION")
                if "NORMAL" not in attributes:
                    raise ValueError("indexed cache needs NORMAL attributes")
                pos = _read_accessor(doc, blob, attributes["POSITION"])
                nrm = _read_accessor(doc, blob, attributes["NORMAL"])
                tex = (_read_accessor(doc, blob, attributes["TEXCOORD_0"])
                       if "TEXCOORD_0" in attributes else [])
                count = len(pos) // 3
                if len(nrm) != 3 * count:
                    raise ValueError("NORMAL count != POSITION count")
                if tex and len(tex) != 2 * count:
                    raise ValueError("TEXCOORD_0 count != POSITION count")
                if "indices" in primitive:
                    idx = [int(v) for v in _read_accessor(
                        doc, blob, primitive["indices"])]
                else:
                    idx = list(range(count))
                if len(idx) % 3 or any(not (0 <= i < count) for i in idx):
                    raise ValueError("glb has invalid indices")
                base = len(positions) // 3
                for i in range(count):
                    positions.extend(_apply_point(matrix, pos[i * 3:i * 3 + 3]))
                    normals.extend(_apply_normal(norm_matrix,
                                                 nrm[i * 3:i * 3 + 3]))
                    if tex:
                        uvs.extend((tex[i * 2], tex[i * 2 + 1]))
                    else:
                        uvs.extend((0.0, 0.0))
                indices.extend(base + i for i in idx)
        for child in node.get("children", []):
            walk(int(child), matrix)

    scene = doc.get("scenes", [{}])[doc.get("scene", 0)]
    for root in scene.get("nodes", []):
        walk(int(root), _identity())
    return positions, normals, uvs, indices


def glb_to_zbrd(data: bytes, part: zboard.Part) -> bytes:
    positions, normals, uvs, indices = glb_indexed(data)
    if not indices:
        raise ValueError("glb has no triangles")
    mesh = zboard.MeshBuilder(positions, normals, uvs, indices)
    return zboard.encode([zboard.Part(part.name, part.kind, part.lod_mask,
                                      part.material, part.tag, part.color, mesh)])


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def glb_embedded_texture(data: bytes) -> bytes:
    """抽出 glb 内嵌的第一张 baseColor 贴图，返回未经重编码的 PNG 字节。

    混元产物的 images[0] 恒为 inline PNG bufferView。抽成独立文件是为了让
    Unity（运行时只读 StreamingAssets 文件）/ Web（GLTFLoader 解码会挂起）/
    Blender 三端都能拿到**同一份**字节并用 sha256 锁定，而不是各自解析 glb。
    """
    doc, blob = _split_glb(data)
    images = doc.get("images") or []
    if len(images) != 1:
        raise ValueError("expected exactly one embedded image, got %d"
                         % len(images))
    image = images[0]
    if image.get("mimeType") != "image/png" or "bufferView" not in image:
        raise ValueError("embedded image must be an inline PNG bufferView")
    view = doc["bufferViews"][image["bufferView"]]
    start = int(view.get("byteOffset", 0))
    png = blob[start:start + int(view["byteLength"])]
    if not png.startswith(PNG_MAGIC):
        raise ValueError("embedded image is not a PNG")
    return png


def suggest_pivot(lo: Sequence[float],
                  hi: Sequence[float]) -> Tuple[float, float, float]:
    """把 bbox 的 XZ 中心与底面作为资产原点的建议值（地面物常用）。"""
    cx = (float(lo[0]) + float(hi[0])) / 2
    cz = (float(lo[2]) + float(hi[2])) / 2
    return (0.0 if abs(cx) < PIVOT_EPSILON else round(cx, 6),
            float(lo[1]),
            0.0 if abs(cz) < PIVOT_EPSILON else round(cz, 6))


def suggest_center(lo: Sequence[float],
                   hi: Sequence[float]) -> Tuple[float, float, float]:
    """把 bbox 的体心作为资产原点（悬浮物 / 环 / 灯笼等锚点在中心的物体）。"""
    cx = (float(lo[0]) + float(hi[0])) / 2
    cy = (float(lo[1]) + float(hi[1])) / 2
    cz = (float(lo[2]) + float(hi[2])) / 2
    return (0.0 if abs(cx) < PIVOT_EPSILON else round(cx, 6),
            0.0 if abs(cy) < PIVOT_EPSILON else round(cy, 6),
            0.0 if abs(cz) < PIVOT_EPSILON else round(cz, 6))


def _rotate_y(point: Sequence[float], degrees: float) -> Tuple[float, float, float]:
    rad = math.radians(degrees)
    c, s = math.cos(rad), math.sin(rad)
    return (point[0] * c + point[2] * s, point[1],
            -point[0] * s + point[2] * c)


def world_bounds(lo: Sequence[float], hi: Sequence[float],
                 anchor: Sequence[float], rotation_y_deg: float,
                 scale: float, pivot: Sequence[float]) -> Dict[str, List[float]]:
    """world = anchor + rotY * scale * (local - pivot_offset)。

    代理资产的 pivot 恒为 0，所以直接等于 ZBRD 里的世界包围盒；
    混元资产可显式给 pivot，把自带原点平移到「底面中心」。
    """
    corners = []
    for x in (lo[0], hi[0]):
        for y in (lo[1], hi[1]):
            for z in (lo[2], hi[2]):
                local = (x - pivot[0], y - pivot[1], z - pivot[2])
                rotated = _rotate_y(local, rotation_y_deg)
                corners.append((rotated[0] * scale + anchor[0],
                                rotated[1] * scale + anchor[1],
                                rotated[2] * scale + anchor[2]))
    return {"min": [min(c[a] for c in corners) for a in range(3)],
            "max": [max(c[a] for c in corners) for a in range(3)]}


# --------------------------------------------------------------------------
# 契约读取 / manifest
# --------------------------------------------------------------------------

def style_items(pack: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    return {item["id"]: item for section in ("decorations", "interactables")
            for item in pack.get(section, [])}


def _part_name(style_id: str, item: Dict[str, Any]) -> str:
    prefix = ("interactable_" if item.get("tag") else "decoration_")
    return prefix + style_id


def build_manifest(pack: Dict[str, Any], spec: Dict[str, Any],
                   root: str = ROOT) -> Dict[str, Any]:
    contract = pack["external_mesh_contract"]
    items = style_items(pack)
    assets: List[Dict[str, Any]] = []
    asset_bounds: Dict[str, Tuple[Tuple[float, float, float],
                                  Tuple[float, float, float]]] = {}
    pivots: Dict[str, List[float]] = {}
    for entry in contract["items"]:
        record: Dict[str, Any] = {
            "asset_id": entry["asset_id"],
            "label": entry.get("label", ""),
            "status": entry["status"],
            "asset_origin": entry.get("asset_origin"),
            "mesh": entry.get("mesh"),
            "mesh_sha256": entry.get("mesh_sha256"),
            "cache": entry.get("cache"),
            "cache_sha256": entry.get("cache_sha256"),
            "texture": entry.get("texture"),
            "texture_sha256": entry.get("texture_sha256"),
        }
        if entry["status"] == "asset_ready":
            path = os.path.join(root, entry["mesh"].replace("/", os.sep))
            data = read_bytes(path)
            record["bytes"] = len(data)
            record["triangles"] = glb_triangles(data)[3]
            lo, hi = glb_bounds(data)
            record["bounds"] = {"min": list(lo), "max": list(hi)}
            pivot = [float(v) for v in entry.get("pivot_offset", [0.0, 0.0, 0.0])]
            record["pivot_offset"] = pivot
            assets.append(record)
            asset_bounds[entry["asset_id"]] = (lo, hi)
            pivots[entry["asset_id"]] = pivot
        else:
            assets.append(record)

    placements: List[Dict[str, Any]] = []
    for entry in contract["items"]:
        for instance in entry["instances"]:
            style_id = instance["id"]
            item = items[style_id]
            ready = entry["status"] == "asset_ready"
            record = {
                "asset_id": entry["asset_id"],
                "instance_id": style_id,
                "proxy_part": _part_name(style_id, item),
                "zone": instance["zone"],
                "anchor": [float(v) for v in instance["anchor"]],
                "rotation_y_deg": float(instance.get("rotation_y_deg", 0.0)),
                "scale": float(instance.get("scale", 1.0)),
                "material": "zboard_" + style_id,
                "tag": item.get("tag", ""),
                "asset_ready": ready,
            }
            if ready:
                lo, hi = asset_bounds[entry["asset_id"]]
                record["world_bounds"] = world_bounds(
                    lo, hi, record["anchor"], record["rotation_y_deg"],
                    record["scale"], pivots[entry["asset_id"]])
            placements.append(record)

    return {
        "schema": MANIFEST_SCHEMA,
        "generated_by": "external_mesh.py",
        "contract_schema": contract["schema"],
        "asset_format": "glb",
        "unity_cache_format": "ZBRD v1 (single part)",
        "coordinate_system": ("glTF Y-up right-handed; world = anchor + rotY * "
                              "scale * (local - pivot_offset); proxy assets use "
                              "pivot_offset [0,0,0] and are already anchor-relative"),
        "assets": assets,
        "placements": placements,
        "summary": {
            "assets": len(assets),
            "asset_ready": len([a for a in assets
                                if a["status"] == "asset_ready"]),
            "placements": len(placements),
            "consumed": len([p for p in placements if p["asset_ready"]]),
        },
    }


def write_manifest(pack: Dict[str, Any], spec: Dict[str, Any],
                   root: str = ROOT, build_root: str = BUILD) -> Dict[str, Any]:
    manifest = build_manifest(pack, spec, root)
    os.makedirs(build_root, exist_ok=True)
    with open(os.path.join(build_root, MANIFEST_NAME), "w",
              encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    return manifest


# --------------------------------------------------------------------------
# 代理资产（先把管线打通；混元资产就绪后只替换 glb + sha256）
# --------------------------------------------------------------------------

def proxy_glb(asset_id: str, pack: Dict[str, Any],
              visual: Dict[str, Any]) -> bytes:
    contract = pack["external_mesh_contract"]
    entry = next(e for e in contract["items"] if e["asset_id"] == asset_id)
    items = style_items(pack)
    style_id = entry["instances"][0]["id"]
    item = items[style_id]
    mesh = zboard.build_primitive(item, place=False)
    color = (visual.get("materials", {}).get("zboard_" + style_id, {})
             .get("color", item.get("color") or [0.5, 0.5, 0.5, 1.0]))
    return write_mesh_glb(asset_id, "zboard_" + style_id, color, mesh,
                          {"asset_id": asset_id, "instances":
                           [i["id"] for i in entry["instances"]],
                           "asset_origin": "proxy_zbrd"})


def export_proxies(style_path: str = STYLE_PATH,
                   visual_path: str = VISUAL_PATH,
                   root: str = ROOT) -> Dict[str, Any]:
    with open(style_path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    with open(SPEC_PATH, "r", encoding="utf-8") as handle:
        spec = json.load(handle)
    with open(visual_path, "r", encoding="utf-8") as handle:
        visual = json.load(handle)
    zboard.validate_style_pack(pack, spec)
    out_dir = os.path.join(root, PROXY_DIR.replace("/", os.sep))
    os.makedirs(out_dir, exist_ok=True)
    items = style_items(pack)
    for entry in pack["external_mesh_contract"]["items"]:
        asset_id = entry["asset_id"]
        style_id = entry["instances"][0]["id"]
        item = items[style_id]
        glb = proxy_glb(asset_id, pack, visual)
        lo, hi = glb_bounds(glb)
        part = zboard.Part(
            _part_name(style_id, item),
            zboard.KIND_INTERACTABLE if item.get("tag") else zboard.KIND_DECORATION,
            zboard.lod_mask_for(item["min_lod"]),
            "zboard_" + style_id, item.get("tag", ""),
            zboard._clamp_color(  # noqa: SLF001
                item.get("color") or [0.5, 0.5, 0.5, 1.0]),
            zboard.build_primitive(item, place=False))
        cache = glb_to_zbrd(glb, part)
        glb_name = asset_id + ".glb"
        cache_name = asset_id + ".zbrd"
        with open(os.path.join(out_dir, glb_name), "wb") as handle:
            handle.write(glb)
        with open(os.path.join(out_dir, cache_name), "wb") as handle:
            handle.write(cache)
        entry.update({
            "status": "asset_ready",
            "asset_origin": "proxy_zbrd",
            "mesh": PROXY_DIR + "/" + glb_name,
            "mesh_sha256": sha256_bytes(glb),
            "cache": PROXY_DIR + "/" + cache_name,
            "cache_sha256": sha256_bytes(cache),
            # 代理 glb 的局部坐标已经是「锚点相对」的，不再二次平移。
            "pivot_offset": [0.0, 0.0, 0.0],
        })
    zboard.validate_external_mesh_contract(pack, spec, root)
    with open(style_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(pack, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    return write_manifest(pack, spec, root)


# --------------------------------------------------------------------------
# 外部 glb 接管（混元 3D / 手工）：原地替换 glb + 派生缓存 + 重算 pivot / scale
# --------------------------------------------------------------------------

def image_bytes(doc: Dict[str, Any]) -> int:
    """glb 内嵌贴图的总字节数（只看 bufferView 内嵌，uri 外链算 0）。"""
    total = 0
    for image in doc.get("images", []):
        view = image.get("bufferView")
        if isinstance(view, int):
            total += int(doc["bufferViews"][view]["byteLength"])
    return total


def _downscale_image(raw: bytes, mime_type: str, max_size: int) -> bytes:
    """把单张内嵌贴图降采样到最长边不超过 max_size 像素。已达标或解码失败时
    返回原字节（bytes 不可变，原样返回就是同一个对象，调用方可用 != 判断）。"""
    import io as _io
    from PIL import Image
    if not str(mime_type).startswith("image/"):
        return raw
    try:
        image = Image.open(_io.BytesIO(raw))
    except Exception:  # noqa: BLE001 —— 非可识别图像就原样保留，绝不弄坏 glb
        return raw
    width, height = image.size
    if max(width, height) <= max_size:
        return raw
    scale = max_size / max(width, height)
    new_size = (max(1, int(width * scale)), max(1, int(height * scale)))
    resized = image.resize(new_size, Image.Resampling.LANCZOS)
    buffer = _io.BytesIO()
    if "jpeg" in str(mime_type):
        resized.convert("RGB").save(buffer, format="JPEG", quality=90)
    else:
        resized.save(buffer, format="PNG")
    return buffer.getvalue()


def resize_glb_images(data: bytes, max_size: int) -> bytes:
    """把 glb 内嵌贴图降采样到最长边不超过 max_size 像素并重打包。

    几何 / 材质 / 节点 / accessor / sampler 全都不动，只把 image 对应的
    bufferView 字节换成降采样后的结果，并重排 BIN chunk（每个 bufferView 重新
    4 字节对齐）。没有任何一张贴图被缩小时返回原字节（sha256 不变）。
    """
    doc, blob = _split_glb(data)
    views = doc.get("bufferViews", [])
    images = doc.get("images", [])
    if not images:
        return data
    replacements: Dict[int, bytes] = {}
    for image in images:
        view = image.get("bufferView")
        mime = image.get("mimeType", "")
        if not isinstance(view, int) or not str(mime).startswith("image/"):
            continue
        raw = blob[views[view]["byteOffset"]:
                   views[view]["byteOffset"] + views[view]["byteLength"]]
        new_raw = _downscale_image(raw, str(mime), max_size)
        if new_raw != raw:
            replacements[view] = new_raw
    if not replacements:
        return data
    binary = bytearray()
    for i, view in enumerate(views):
        raw = replacements.get(i)
        if raw is None:
            raw = blob[view["byteOffset"]:view["byteOffset"] + view["byteLength"]]
        view["byteOffset"] = len(binary)
        view["byteLength"] = len(raw)
        binary.extend(raw)
        binary.extend(b"\x00" * (-len(raw) % 4))
    doc["buffers"][0]["byteLength"] = len(binary)
    mod = _glb_module()
    payload = json.dumps(doc, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
    json_chunk = payload + b" " * (-len(payload) % 4)
    bin_chunk = bytes(binary)
    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    return b"".join((
        struct.pack("<3I", mod.GLB_MAGIC, 2, total),
        struct.pack("<2I", len(json_chunk), mod.CHUNK_JSON), json_chunk,
        struct.pack("<2I", len(bin_chunk), mod.CHUNK_BIN), bin_chunk,
    ))


def _install_asset(entry: Dict[str, Any], pack: Dict[str, Any],
                   spec: Dict[str, Any], data: bytes, origin: str,
                   height: float | None, diameter: float | None,
                   pivot_center: bool, style_path: str,
                   root: str) -> Dict[str, Any]:
    """import / resize 共用的尾部：度量 → 写 mesh → 派生缓存 → 回写契约 + 清单。

    任何一步失败都不会把半成品契约写进 style-pack.json（契约在最后才落盘）。
    """
    asset_id = entry["asset_id"]
    items = style_items(pack)
    style_id = entry["instances"][0]["id"]
    item = items[style_id]
    doc, _blob = _split_glb(data)
    _positions, _normals, _uvs, triangles = glb_triangles(data)
    if not triangles:
        raise ValueError("`%s` glb has no triangles" % asset_id)
    lo, hi = glb_bounds(data)
    size = [hi[a] - lo[a] for a in range(3)]
    pivot = [round(v, 6) for v in (suggest_center(lo, hi) if pivot_center
                                   else suggest_pivot(lo, hi))]

    dest = os.path.join(root, entry["mesh"].replace("/", os.sep))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as handle:
        handle.write(data)

    scale = None
    if height:
        if size[1] <= PIVOT_EPSILON:
            raise ValueError("`%s` is flat on Y (height %.6f); use --diameter"
                             % (asset_id, size[1]))
        scale = round(float(height) / size[1], 4)
    elif diameter:
        width = max(size[0], size[2])
        if width <= PIVOT_EPSILON:
            raise ValueError("`%s` is degenerate on XZ; use --height" % asset_id)
        scale = round(float(diameter) / width, 4)
    if scale:
        for instance in entry["instances"]:
            instance["scale"] = scale

    part = zboard.Part(
        _part_name(style_id, item),
        zboard.KIND_INTERACTABLE if item.get("tag") else zboard.KIND_DECORATION,
        zboard.lod_mask_for(item["min_lod"]),
        "zboard_" + style_id, item.get("tag", ""),
        zboard._clamp_color(  # noqa: SLF001
            item.get("color") or [0.5, 0.5, 0.5, 1.0]),
        zboard.build_primitive(item, place=False))
    cache = glb_to_zbrd(data, part)
    cache_path = os.path.join(root, entry["cache"].replace("/", os.sep))
    os.makedirs(os.path.dirname(cache_path), exist_ok=True)
    with open(cache_path, "wb") as handle:
        handle.write(cache)

    # 内嵌 baseColor 贴图同步抽成独立 PNG：接管 / 降采样改了 glb 就必须重抽，
    # 否则三端会拿到与 glb 不一致的贴图（verify() 会逐字节比对）。
    texture = glb_embedded_texture(data)
    texture_relative = "assets/external_meshes/%s.png" % asset_id
    texture_path = os.path.join(root, texture_relative.replace("/", os.sep))
    os.makedirs(os.path.dirname(texture_path), exist_ok=True)
    with open(texture_path, "wb") as handle:
        handle.write(texture)

    entry.update({
        "status": "asset_ready",
        "asset_origin": origin,
        "mesh_sha256": sha256_bytes(data),
        "cache_sha256": sha256_bytes(cache),
        "texture": texture_relative,
        "texture_sha256": sha256_bytes(texture),
        "pivot_offset": pivot,
    })
    zboard.validate_external_mesh_contract(pack, spec, root)
    with open(style_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(pack, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    write_manifest(pack, spec, root)
    return {
        "asset_id": asset_id,
        "origin": origin,
        "mesh": entry["mesh"],
        "bytes": len(data),
        "triangles": triangles,
        "bounds": {"min": list(lo), "max": list(hi)},
        "size": size,
        "pivot_offset": pivot,
        "scale": scale,
        "cache_bytes": len(cache),
        "texture_bytes": image_bytes(doc),
        "images": len(doc.get("images", [])),
    }


def refresh_caches(style_path: str = STYLE_PATH,
                   root: str = ROOT) -> List[Dict[str, Any]]:
    """按现有 glb 重新派生 Unity 的 ZBRD 缓存并回写 cache_sha256。

    glb 字节 / mesh_sha256 / pivot / scale 一律不动，只用于缓存格式升级
    （例如从「三角形汤」改成索引网格）。改完必须重跑 `zboard.py --variants`
    同步三端清单与场景，否则 verify() 会报 manifest stale。
    """
    pack = _load_pack(style_path)
    spec = _load_spec()
    items = style_items(pack)
    report: List[Dict[str, Any]] = []
    for entry in pack["external_mesh_contract"]["items"]:
        if entry["status"] != "asset_ready" or not entry.get("cache"):
            continue
        asset_id = entry["asset_id"]
        data = read_bytes(os.path.join(root, entry["mesh"].replace("/", os.sep)))
        if sha256_bytes(data) != entry["mesh_sha256"]:
            raise ValueError("`%s` glb sha256 drifted" % asset_id)
        style_id = entry["instances"][0]["id"]
        item = items[style_id]
        part = zboard.Part(
            _part_name(style_id, item),
            zboard.KIND_INTERACTABLE if item.get("tag") else zboard.KIND_DECORATION,
            zboard.lod_mask_for(item["min_lod"]),
            "zboard_" + style_id, item.get("tag", ""),
            zboard._clamp_color(  # noqa: SLF001
                item.get("color") or [0.5, 0.5, 0.5, 1.0]),
            zboard.MeshBuilder())
        cache = glb_to_zbrd(data, part)
        with open(os.path.join(root, entry["cache"].replace("/", os.sep)),
                  "wb") as handle:
            handle.write(cache)
        entry["cache_sha256"] = sha256_bytes(cache)
        decoded = zboard.decode_zbrd(cache)[0].mesh
        report.append({"asset_id": asset_id, "cache_bytes": len(cache),
                       "vertices": decoded.vertex_count,
                       "indices": len(decoded.indices)})
    # 契约校验按新 sha 重新比对缓存文件，通过后才把契约落盘。
    zboard.validate_external_mesh_contract(pack, spec, root)
    with open(style_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(pack, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    write_manifest(pack, spec, root)
    return report


def extract_textures(style_path: str = STYLE_PATH,
                     root: str = ROOT) -> List[Dict[str, Any]]:
    """把 glb 内嵌 baseColor PNG 抽成独立文件并写进契约（三端共用真源）。

    Web 不能走 GLTFLoader 的贴图解码（ImageBitmapLoader 会挂起），Unity
    运行时只读 StreamingAssets 文件，Blender 可直接换成同一份 PNG —— 三端
    共用独立文件 + sha256，避免各自解析 glb 产生口径分叉。
    """
    pack = _load_pack(style_path)
    spec = _load_spec()
    report: List[Dict[str, Any]] = []
    for entry in pack["external_mesh_contract"]["items"]:
        if entry["status"] != "asset_ready":
            continue
        asset_id = entry["asset_id"]
        data = read_bytes(os.path.join(root, entry["mesh"].replace("/", os.sep)))
        if sha256_bytes(data) != entry["mesh_sha256"]:
            raise ValueError("`%s` glb sha256 drifted" % asset_id)
        png = glb_embedded_texture(data)
        relative = "assets/external_meshes/%s.png" % asset_id
        path = os.path.join(root, relative.replace("/", os.sep))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(png)
        entry["texture"] = relative
        entry["texture_sha256"] = sha256_bytes(png)
        report.append({"asset_id": asset_id, "texture": relative,
                       "bytes": len(png)})
    zboard.validate_external_mesh_contract(pack, spec, root)
    with open(style_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(pack, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    write_manifest(pack, spec, root)
    return report


def import_asset(asset_id: str, source: str, origin: str = "hunyuan_3d",
                 height: float | None = None, diameter: float | None = None,
                 texture_size: int | None = None, pivot_center: bool = False,
                 style_path: str = STYLE_PATH,
                 root: str = ROOT) -> Dict[str, Any]:
    """把外部来源的 glb 接管成契约里的正式资产。

    做的事（顺序有意义）：
      1. 解析 + 度量来源 glb（三角面、包围盒、内嵌贴图字节数）；
      2. `texture_size` 给定时，把内嵌贴图降采样到该最长边（PIL，重打包 glb）；
         否则 glb 字节原样、不烘焙节点变换；
      3. 写到契约声明的 `mesh` 路径；
      4. `pivot_offset` = 「底面中心」；`pivot_center=True` 时改用「体心」
         （悬浮物 / 环 / 灯笼这类锚点在中心的物体）；
      5. `height` 把资产高度归一到该米数、`diameter` 把 XZ 直径归一到该米数，
         二选一，写进每个 instance 的 scale；
      6. 用同一份 glb 派生 ZBRD 单部件缓存，回写两份 sha256 与 manifest。

    节点自带的旋转/缩放由 `glb_triangles` 在转换时乘进去（混元的 Z-up 网格
    靠 `nodes[0].rotation` 的 +90°X 转成 glTF Y-up），所以包围盒与 pivot 都在
    **节点变换之后**的局部空间里，消费端不需要再处理节点变换。

    来源 glb 与目标路径不同时，接管成功后那份「下载落盘副本」可以删掉：
    契约里的 `mesh_sha256` 就是最终交付产物的指纹，`tools/hunyuan3d.py` 也能按
    参考图重出一份。仓库里不要留两份同一份资产。
    """
    if origin not in zboard.ASSET_ORIGINS:
        raise ValueError("asset_origin must be one of %s"
                         % ", ".join(zboard.ASSET_ORIGINS))
    with open(style_path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    spec = _load_spec()
    zboard.validate_style_pack(pack, spec)
    contract = pack["external_mesh_contract"]
    entry = next((e for e in contract["items"] if e["asset_id"] == asset_id), None)
    if entry is None:
        raise ValueError("unknown external asset `%s`" % asset_id)
    data = read_bytes(source)
    if texture_size and texture_size > 0:
        data = resize_glb_images(data, texture_size)
    return _install_asset(entry, pack, spec, data, origin, height, diameter,
                          pivot_center, style_path, root)


def resize_asset(asset_id: str, max_size: int,
                 style_path: str = STYLE_PATH,
                 root: str = ROOT) -> Dict[str, Any]:
    """把已接管资产的 glb 内嵌贴图降采样到最长边不超过 max_size，并重派生缓存。

    只改贴图字节（改 sha256），几何 / pivot / scale 全都不动。改完必须重跑
    `external_mesh.py --check` 与 `zboard.py --variants` 让三端清单同步。
    """
    with open(style_path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    spec = _load_spec()
    zboard.validate_style_pack(pack, spec)
    contract = pack["external_mesh_contract"]
    entry = next((e for e in contract["items"] if e["asset_id"] == asset_id), None)
    if entry is None:
        raise ValueError("unknown external asset `%s`" % asset_id)
    origin = entry.get("asset_origin")
    if origin not in zboard.ASSET_ORIGINS:
        raise ValueError("asset `%s` has no valid asset_origin" % asset_id)
    data = read_bytes(os.path.join(root, entry["mesh"].replace("/", os.sep)))
    data = resize_glb_images(data, max_size)
    return _install_asset(entry, pack, spec, data, origin, None, None,
                          False, style_path, root)


# --------------------------------------------------------------------------
# 校验
# --------------------------------------------------------------------------

def verify(pack: Dict[str, Any], spec: Dict[str, Any],
           root: str = ROOT) -> Dict[str, Any]:
    zboard.validate_style_pack(pack, spec)
    zboard.validate_external_mesh_contract(pack, spec, root)
    items = style_items(pack)
    report: Dict[str, Any] = {"assets": 0, "consumed": 0, "placements": 0,
                              "triangles": 0, "bytes": 0}
    pivots: Dict[str, List[float]] = {}
    for entry in pack["external_mesh_contract"]["items"]:
        if entry["status"] != "asset_ready":
            continue
        asset_id = entry["asset_id"]
        glb_path = os.path.join(root, entry["mesh"].replace("/", os.sep))
        glb = read_bytes(glb_path)
        if sha256_bytes(glb) != entry["mesh_sha256"]:
            raise ValueError("`%s` glb sha256 drifted" % asset_id)
        _doc, _blob = _split_glb(glb)
        lo, hi = glb_bounds(glb)
        if entry.get("texture"):
            texture = read_bytes(os.path.join(
                root, entry["texture"].replace("/", os.sep)))
            if sha256_bytes(texture) != entry["texture_sha256"]:
                raise ValueError("`%s` baseColor texture sha256 drifted" % asset_id)
            # 独立文件必须与 glb 内嵌字节完全一致，否则三端真源会分叉。
            if texture != glb_embedded_texture(glb):
                raise ValueError("`%s` texture != glb embedded PNG; "
                                 "re-run --extract-textures" % asset_id)
            report["bytes"] += len(texture)
        pivot = [float(v) for v in entry.get("pivot_offset", [0.0, 0.0, 0.0])]
        pivots[asset_id] = list(pivot)
        if (entry.get("asset_origin") == "proxy_zbrd"
                and any(abs(v) > PIVOT_EPSILON for v in pivot)):
            raise ValueError("proxy `%s` must keep pivot_offset [0,0,0]"
                             % asset_id)
        if entry.get("cache"):
            cache_path = os.path.join(root, entry["cache"].replace("/", os.sep))
            cache = read_bytes(cache_path)
            if sha256_bytes(cache) != entry["cache_sha256"]:
                raise ValueError("`%s` mesh cache sha256 drifted" % asset_id)
            cached = zboard.decode_zbrd(cache)
            if len(cached) != 1:
                raise ValueError("`%s` cache must hold exactly one part" % asset_id)
            part = cached[0]
            expected = items[entry["instances"][0]["id"]]
            if part.name != _part_name(expected["id"], expected):
                raise ValueError("`%s` cache part name drifted" % asset_id)
            if part.material != "zboard_" + expected["id"]:
                raise ValueError("`%s` cache material drifted" % asset_id)
            cache_lo, cache_hi = part.mesh.bounds()
            for a in range(3):
                if (abs(cache_lo[a] - lo[a]) > 1.0e-3
                        or abs(cache_hi[a] - hi[a]) > 1.0e-3):
                    raise ValueError("`%s` cache bounds != glb bounds" % asset_id)
            report["triangles"] += len(part.mesh.indices) // 3
        report["assets"] += 1
        report["consumed"] += len(entry["instances"])
        report["placements"] += len(entry["instances"])
        report["bytes"] += len(glb)

    manifest_path = os.path.join(BUILD, MANIFEST_NAME)
    if os.path.isfile(manifest_path):
        with open(manifest_path, "r", encoding="utf-8") as handle:
            landed = json.load(handle)
        fresh = build_manifest(pack, spec, root)
        if landed != fresh:
            raise ValueError("build/external_mesh.manifest.json is stale; "
                             "re-run `python zboard.py --variants`")
    report["pivots"] = pivots
    return report


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="glb + sha256 + 锚点的三端共享消费层")
    parser.add_argument("--style", default=STYLE_PATH)
    parser.add_argument("--visual", default=VISUAL_PATH)
    parser.add_argument("--write-proxies", action="store_true",
                        help="用 ZBRD 代理原语重出 glb + Unity 缓存并回写契约")
    parser.add_argument("--check", action="store_true",
                        help="只校验已有 glb / 缓存 / manifest")
    parser.add_argument("--refresh-caches", action="store_true",
                        help="按现有 glb 重派生 Unity ZBRD 缓存并回写 cache_sha256")
    parser.add_argument("--extract-textures", action="store_true",
                        help="把 glb 内嵌 baseColor PNG 抽成独立贴图并写进契约")
    parser.add_argument("--manifest-only", action="store_true",
                        help="只刷新 build/external_mesh.manifest.json")
    parser.add_argument("--import-glb", metavar="ASSET_ID", default="",
                        help="把外部 glb 接管成该资产的 mesh（字节原样）")
    parser.add_argument("--source", default="", help="--import-glb 的来源 glb")
    parser.add_argument("--origin", default="hunyuan_3d",
                        choices=list(zboard.ASSET_ORIGINS),
                        help="--import-glb 写入的 asset_origin")
    parser.add_argument("--height", type=float, default=0.0,
                        help="--import-glb 把资产高度归一到该米数（写进 scale）")
    parser.add_argument("--diameter", type=float, default=0.0,
                        help="--import-glb 把 XZ 直径归一到该米数（环/灯笼用）")
    parser.add_argument("--pivot-center", action="store_true",
                        help="--import-glb 用体心作 pivot（悬浮物），默认底面中心")
    parser.add_argument("--texture-size", type=int, default=0,
                        help="--import-glb 把内嵌贴图降采样到该最长边（像素）")
    parser.add_argument("--resize-texture", metavar="ASSET_ID", default="",
                        help="把已接管资产的内嵌贴图降采样（需配 --texture-size）")
    args = parser.parse_args(argv)
    try:
        if args.refresh_caches:
            for item in refresh_caches(args.style):
                print("CACHE %-14s %6d vertices / %6d indices / %.2f MB"
                      % (item["asset_id"], item["vertices"], item["indices"],
                         item["cache_bytes"] / (1 << 20)))
            report = verify(_load_pack(args.style), _load_spec())
            print("EXTERNAL OK: %d assets / %d triangles"
                  % (report["assets"], report["triangles"]))
            return 0
        if args.extract_textures:
            for item in extract_textures(args.style):
                print("TEXTURE %-14s %s / %.2f MB"
                      % (item["asset_id"], item["texture"],
                         item["bytes"] / (1 << 20)))
            report = verify(_load_pack(args.style), _load_spec())
            print("EXTERNAL OK: %d assets / %d triangles"
                  % (report["assets"], report["triangles"]))
            return 0
        if args.resize_texture:
            if args.texture_size <= 0:
                raise ValueError("--resize-texture needs --texture-size <px>")
            report = resize_asset(args.resize_texture, args.texture_size,
                                  args.style)
            print("RESIZED %s (%s) texture=%.2f MB (%d image) -> %.2f MB glb"
                  % (report["asset_id"], report["origin"],
                     report["texture_bytes"] / (1 << 20), report["images"],
                     report["bytes"] / (1 << 20)))
            report = verify(_load_pack(args.style), _load_spec())
            print("EXTERNAL OK: %d assets / %d placements / %d triangles / "
                  "%d bytes" % (report["assets"], report["placements"],
                                report["triangles"], report["bytes"]))
            return 0
        if args.import_glb:
            if not args.source:
                raise ValueError("--import-glb needs --source <glb>")
            report = import_asset(args.import_glb, args.source, args.origin,
                                  args.height or None, args.diameter or None,
                                  args.texture_size or None, args.pivot_center,
                                  args.style)
            print("IMPORTED %s (%s) %d tris / %.2f MB / bounds %s -> %s"
                  % (report["asset_id"], report["origin"],
                     report["triangles"], report["bytes"] / (1 << 20),
                     ["%.4f" % v for v in report["bounds"]["min"]],
                     ["%.4f" % v for v in report["bounds"]["max"]]))
            print("  pivot_offset=%s scale=%s cache=%.2f MB texture=%.2f MB "
                  "(%d image)" % (report["pivot_offset"], report["scale"],
                                  report["cache_bytes"] / (1 << 20),
                                  report["texture_bytes"] / (1 << 20),
                                  report["images"]))
            if report["texture_bytes"] > (4 << 20):
                print("  NOTE: 内嵌贴图 %.2f MB 且三端运行时都不消费它（Unity 走 "
                      "zboard_<id> 材质，ZBRD 缓存不带贴图），按需降采样后再接管。"
                      % (report["texture_bytes"] / (1 << 20)))
            report = verify(_load_pack(args.style), _load_spec())
            print("EXTERNAL OK: %d assets / %d placements / %d triangles / "
                  "%d bytes" % (report["assets"], report["placements"],
                                report["triangles"], report["bytes"]))
            return 0
        if args.write_proxies:
            manifest = export_proxies(args.style, args.visual)
            report = verify(_load_pack(args.style), _load_spec())
            print("EXTERNAL OK: %d assets / %d placements / %d triangles / "
                  "%d bytes" % (report["assets"], report["placements"],
                                report["triangles"], report["bytes"]))
            print("MANIFEST %s (%d placements)"
                  % (MANIFEST_NAME, len(manifest["placements"])))
            return 0
        pack = _load_pack(args.style)
        spec = _load_spec()
        if args.check:
            report = verify(pack, spec)
            manifest = build_manifest(pack, spec)
            print("EXTERNAL OK: %d assets / %d placements / %d triangles / "
                  "%d bytes" % (report["assets"], manifest["summary"]["placements"],
                                report["triangles"], report["bytes"]))
            for asset_id, pivot in sorted(report["pivots"].items()):
                print("  %-14s pivot=%s" % (asset_id, pivot))
            return 0
        manifest = write_manifest(pack, spec)
        print("MANIFEST %s (%d assets / %d placements)"
              % (MANIFEST_NAME, manifest["summary"]["assets"],
                 manifest["summary"]["placements"]))
        return 0
    except (OSError, ValueError, KeyError, zboard.SpecError,
            json.JSONDecodeError) as exc:
        print("EXTERNAL FAILED: %s" % exc)
        return 1


def _load_pack(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        pack = json.load(handle)
    zboard.validate_external_mesh_contract(pack, _load_spec(),
                                           os.path.dirname(os.path.abspath(path)))
    return pack


def _load_spec() -> Dict[str, Any]:
    with open(SPEC_PATH, "r", encoding="utf-8") as handle:
        spec = json.load(handle)
    zboard.validate_spec(spec)
    return spec


if __name__ == "__main__":
    raise SystemExit(main())
