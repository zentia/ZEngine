"""为美术输出 Blender 可用的 AC90 文件（纯 stdlib，不依赖 bpy/Blender）。

本机没有 Blender 可执行文件，无法直接写二进制 ``.blend``；因此交付三件：

* ``build/blender/AC90_board.glb`` —— glTF 2.0 二进制，15 个 HD 部件各是一个
  独立 Mesh/Node，带 AI 视觉包颜色、法线、UV 与 ZBRD 元数据。Blender 里
  ``File > Import > glTF 2.0`` 直接打开，坐标由 importer 从 Y-up 转到 Z-up。
* ``build/blender/build_ac90.py`` —— bpy 脚本：导入 GLB、套用 AI 天空盒 /
  灯光 / 色彩管理 / Bloom，最后 ``save_as_mainfile`` 产出 ``AC90.blend``。
  美术在 Blender 里 Run Script（或 ``blender -b --python build_ac90.py``）
  即可得到可直接调整的 ``.blend`` 工程。
* ``build/blender/sky_panorama.jpg`` —— AI 全景图，脚本用作 World 环境贴图。

几何唯一真源仍是 ``board.zbrd``；本模块只做格式转换，绝不重写棋盘几何公式。
High/Mid/Low 暂不输出（用户明确「先不管」），只导出 HD 的 15 个部件。

用法：
    python blender_export.py                  # 从 build/ 产物生成
    python blender_export.py --check         # 只校验已有产物
    python zboard.py --variants              # 也会自动调用本模块
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import shutil
import struct
from typing import Any, Dict, List, Sequence

import zboard  # pyright: ignore[reportImplicitRelativeImport]

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(BUILD, "blender")
GLB_NAME = "AC90_board.glb"
SCRIPT_NAME = "build_ac90.py"
SKY_NAME = "sky_panorama.jpg"
BLEND_NAME = "AC90.blend"
BLENDER_SCHEMA = "zengine.chessboard.blender/v1"

GLB_MAGIC = 0x46546C67           # "glTF"
CHUNK_JSON = 0x4E4F534A          # "JSON"
CHUNK_BIN = 0x004E4942           # "BIN\0"
FLOAT = 5126
UINT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

BPY_TEMPLATE = '''"""ZEngine AC90 —— Blender 场景构建脚本（由 blender_export.py 生成，勿手改）。

用法（任选其一）：
  1) Blender > Scripting > Open > build_ac90.py > Run Script
  2) blender --background --python build_ac90.py

脚本导入同目录的 AC90_board.glb（15 个 HD 部件，按 ZBRD part 分对象），
套用 visual-pack.json 的 AI 天空盒 / 灯光 / 色彩管理 / Bloom 参数，最后
另存为同目录的 AC90.blend —— 美术直接打开这个 .blend 调整即可。

坐标：GLB 用 glTF 标准的 Y-up 右手系，Blender 的 glTF importer 自动转成
Z-up；本脚本不再做二次旋转，否则棋盘会躺倒。

USE_WORLD_SKY=True 时用 AI 全景 World 环境贴图当天空，并隐藏几何天穹球的
渲染（视口里仍可见、可编辑）；改成 False 则反过来用天穹球。
"""
import hashlib
import math
import os

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
VISUAL = __ZENGINE_VISUAL_JSON__
EXTERNAL = __ZENGINE_EXTERNAL_JSON__
USE_WORLD_SKY = True

GLB = os.path.join(HERE, "AC90_board.glb")
SKY = os.path.join(HERE, "sky_panorama.jpg")
BLEND = os.path.join(HERE, "AC90.blend")


def set_if(target, key, value):
    """Blender 3.x / 4.x 的 Compositor 与 ViewSettings 字段并不一致。"""
    try:
        setattr(target, key, value)
    except (AttributeError, TypeError, ValueError):
        pass


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for group in (bpy.data.meshes, bpy.data.materials,
                  bpy.data.cameras, bpy.data.lights):
        for item in list(group):
            if item.users == 0:
                group.remove(item)


def organize_collections():
    scene = bpy.context.scene
    board = bpy.data.collections.get("BoardMesh") or bpy.data.collections.new("BoardMesh")
    sky = bpy.data.collections.get("Sky_Dome") or bpy.data.collections.new("Sky_Dome")
    existing = {c.name for c in scene.collection.children}
    for coll in (board, sky):
        if coll.name not in existing:
            scene.collection.children.link(coll)
    for obj in list(scene.objects):
        if obj.type != "MESH":
            continue
        target = sky if "sky" in obj.name.lower() else board
        for old in list(obj.users_collection):
            old.objects.unlink(obj)
        target.objects.link(obj)
        if target is sky and USE_WORLD_SKY:
            obj.hide_render = True


def build_world():
    scene = bpy.context.scene
    world = bpy.data.worlds.new("ZEngine_AI_Sky")
    world.use_nodes = True
    tree = world.node_tree
    tree.nodes.clear()
    env = tree.nodes.new("ShaderNodeTexEnvironment")
    env.projection = "EQUIRECTANGULAR"
    if os.path.isfile(SKY):
        env.image = bpy.data.images.load(SKY, check_existing=True)
    env.location = (-420, 0)
    bg = tree.nodes.new("ShaderNodeBackground")
    bg.inputs["Strength"].default_value = VISUAL["skybox"]["exposure"]
    bg.location = (-160, 0)
    out = tree.nodes.new("ShaderNodeOutputWorld")
    out.location = (80, 0)
    tree.links.new(env.outputs["Color"], bg.inputs["Color"])
    tree.links.new(bg.outputs["Background"], out.inputs["Surface"])
    scene.world = world


def build_lighting():
    light = VISUAL["lighting"]
    data = bpy.data.lights.new("Directional_Key", type="SUN")
    data.energy = light["directional_intensity"]
    data.color = light["directional_color"][:3]
    obj = bpy.data.objects.new("Directional_Key", data)
    bpy.context.scene.collection.objects.link(obj)
    obj.rotation_euler = tuple(
        math.radians(v) for v in light.get("directional_euler", [-38.0, 26.0, 0.0]))
    return obj


def build_camera():
    data = bpy.data.cameras.new("Main Camera")
    data.lens = 50
    obj = bpy.data.objects.new("Main Camera", data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = (0.0, -22.0, 16.0)
    obj.rotation_euler = (Vector((0.0, 0.0, 0.0)) - obj.location).to_track_quat(
        "-Z", "Y").to_euler()
    bpy.context.scene.camera = obj


def apply_color_management():
    view = bpy.context.scene.view_settings
    for name in ("AgX", "Filmic", "Standard"):
        try:
            view.view_transform = name
            break
        except TypeError:
            continue
    set_if(view, "exposure", VISUAL["post_processing"]["exposure_value"])
    set_if(view, "gamma", 1.0)


def apply_bloom():
    scene = bpy.context.scene
    scene.use_nodes = True
    tree = scene.node_tree
    tree.nodes.clear()
    src = tree.nodes.new("CompositorNodeRLayers")
    src.location = (-400, 0)
    glare = tree.nodes.new("CompositorNodeGlare")
    glare.location = (-120, 0)
    bloom = VISUAL["post_processing"]["bloom"]
    set_if(glare, "glare_type", "FOG_GLOW")
    set_if(glare, "threshold", bloom["threshold"])
    set_if(glare, "size", max(6, min(9, int(round(bloom["diffusion"] * 9)))))
    set_if(glare, "mix", -0.85 + min(1.0, bloom["intensity"]) * 0.5)
    dst = tree.nodes.new("CompositorNodeComposite")
    dst.location = (160, 0)
    tree.links.new(src.outputs["Image"], glare.inputs["Image"])
    tree.links.new(glare.outputs["Image"], dst.inputs["Image"])


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def import_external_meshes():
    """按 external_mesh.manifest.json 的锚点摆放混元 / 代理 glb 单体资产。

    锚点与 Unity / Web 完全同源：
        world = anchor + rotY(rotation_y_deg) * scale * (local - pivot_offset)
    glTF 是 Y-up、Blender 是 Z-up，importer 已经转好，这里只换算位置与绕 Z 的
    角度：(x, y, z) -> (x, -z, y)，rotation_euler.z = +rotation_y_deg。
    代理几何与外部单体同时存在时，把代理设为仅线框、不参与渲染。
    """
    pkg = os.path.dirname(HERE)
    assets = {asset["asset_id"]: asset for asset in EXTERNAL.get("assets", [])
              if asset.get("status") == "asset_ready"}
    placements = [p for p in EXTERNAL.get("placements", [])
                  if p.get("asset_ready") and p.get("asset_id") in assets]
    if not placements:
        print("EXTERNAL_SKIP: no asset_ready glb")
        return
    collection = bpy.data.collections.new("ZEngine_External")
    bpy.context.scene.collection.children.link(collection)
    applied = 0
    for placement in placements:
        asset = assets[placement["asset_id"]]
        path = os.path.join(pkg, asset["mesh"].replace("/", os.sep))
        if not os.path.isfile(path) or _sha256_file(path) != asset["mesh_sha256"]:
            print("EXTERNAL_SKIP:", placement["instance_id"], "missing/hash")
            continue
        before = {obj.name for obj in bpy.data.objects}
        bpy.ops.import_scene.gltf(filepath=path)
        imported = [obj for obj in bpy.data.objects if obj.name not in before]
        # glTF importer 会加载 glb 内嵌贴图；这里换成三端共用的独立 PNG，
        # 并按 sha256 校验，保证与 Web / Unity 拿到的是同一份字节。
        # importer 已把 glTF 的 V 向下 UV 转成 Blender 约定，换图无需改 UV。
        texture_rel = asset.get("texture")
        texture_sha = asset.get("texture_sha256")
        if texture_rel and texture_sha:
            texture_path = os.path.join(pkg, texture_rel.replace("/", os.sep))
            if os.path.isfile(texture_path) and _sha256_file(texture_path) == texture_sha:
                image = bpy.data.images.load(texture_path, check_existing=True)
                image.colorspace_settings.name = "sRGB"
                for obj in imported:
                    for slot in obj.material_slots:
                        mat = slot.material
                        if mat is None or not mat.use_nodes:
                            continue
                        for node in mat.node_tree.nodes:
                            if node.type == "TEX_IMAGE":
                                node.image = image
            else:
                print("EXTERNAL_TEX_SKIP:", placement["instance_id"], "missing/hash")
        roots = [obj for obj in imported if obj.parent is None]
        root = roots[0] if roots else (imported[0] if imported else None)
        if root is None:
            print("EXTERNAL_SKIP:", placement["instance_id"], "no object")
            continue
        root.name = "ext_" + placement["instance_id"]
        pivot = asset.get("pivot_offset") or [0.0, 0.0, 0.0]
        anchor = placement["anchor"]
        scale = float(placement.get("scale", 1.0))
        radians = math.radians(float(placement.get("rotation_y_deg", 0.0)))
        cos, sin = math.cos(radians), math.sin(radians)
        offset = (pivot[0] * scale, pivot[1] * scale, pivot[2] * scale)
        dx = offset[0] * cos + offset[2] * sin
        dz = -offset[0] * sin + offset[2] * cos
        root.location = (anchor[0] - dx, -(anchor[2] - dz),
                         anchor[1] - offset[1])
        root.rotation_euler = (0.0, 0.0, radians)
        root.scale = (scale, scale, scale)
        for obj in imported:
            for old in list(obj.users_collection):
                old.objects.unlink(obj)
            collection.objects.link(obj)
        proxy = bpy.data.objects.get(placement.get("proxy_part", ""))
        if proxy is not None:
            proxy.hide_render = True
            proxy.display_type = "WIRE"
        applied += 1
    print("EXTERNAL_OK:", applied, "/", len(placements))


def main():
    clear_scene()
    bpy.ops.import_scene.gltf(filepath=GLB)
    organize_collections()
    import_external_meshes()
    build_world()
    build_lighting()
    build_camera()
    apply_color_management()
    apply_bloom()
    bpy.ops.wm.save_as_mainfile(filepath=BLEND)
    print("ZENGINE BLENDER OK:", BLEND)


if __name__ == "__main__":
    main()
'''


def srgb_to_linear(value: float) -> float:
    """glTF baseColorFactor 是线性值；ZBRD/视觉包里存的是 sRGB 设计值。"""
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def linear_color(color: Sequence[float]) -> List[float]:
    return [srgb_to_linear(float(v)) for v in color[:3]] + [float(color[3])]


class GlbBuilder:
    """最小的 glTF 2.0 / GLB 写入器：只写项目实际用到的字段。"""

    def __init__(self) -> None:
        self.binary = bytearray()
        self.buffer_views: List[Dict[str, Any]] = []
        self.accessors: List[Dict[str, Any]] = []
        self.meshes: List[Dict[str, Any]] = []
        self.materials: List[Dict[str, Any]] = []
        self.nodes: List[Dict[str, Any]] = []
        self.material_index: Dict[str, int] = {}

    def _view(self, raw: bytes, target: int) -> int:
        offset = len(self.binary)
        self.binary.extend(raw)
        self.buffer_views.append({"buffer": 0, "byteOffset": offset,
                                  "byteLength": len(raw), "target": target})
        return len(self.buffer_views) - 1

    def _accessor(self, values: Sequence[float], component: int,
                  kind: str, target: int, bounds: bool = False) -> int:
        fmt = "f" if component == FLOAT else "I"
        raw = struct.pack("<%d%s" % (len(values), fmt), *values)
        dim = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[kind]
        accessor: Dict[str, Any] = {
            "bufferView": self._view(raw, target),
            "componentType": component,
            "count": len(values) // dim,
            "type": kind,
        }
        if bounds:
            accessor["min"] = [min(values[i::dim]) for i in range(dim)]
            accessor["max"] = [max(values[i::dim]) for i in range(dim)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def _material(self, name: str, color: Sequence[float]) -> int:
        if name not in self.material_index:
            self.material_index[name] = len(self.materials)
            self.materials.append({
                "name": name,
                "pbrMetallicRoughness": {
                    "baseColorFactor": linear_color(color),
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.85,
                },
                "doubleSided": name == "sky_gradient",
                "extras": {"zengine_material": name},
            })
        return self.material_index[name]

    def add_part(self, part: zboard.Part, visual: Dict[str, Any]) -> None:
        mesh = part.mesh
        position = self._accessor(mesh.positions, FLOAT, "VEC3",
                                  ARRAY_BUFFER, bounds=True)
        normal = self._accessor(mesh.normals, FLOAT, "VEC3", ARRAY_BUFFER)
        uv = self._accessor(mesh.uvs, FLOAT, "VEC2", ARRAY_BUFFER)
        index = self._accessor(mesh.indices, UINT, "SCALAR",
                               ELEMENT_ARRAY_BUFFER)
        color = visual["materials"].get(part.material, {}).get("color", part.color)
        material = self._material(part.material, color)
        self.meshes.append({
            "name": part.name,
            "primitives": [{
                "attributes": {"POSITION": position, "NORMAL": normal,
                               "TEXCOORD_0": uv},
                "indices": index, "material": material, "mode": 4,
            }],
        })
        self.nodes.append({
            "name": part.name,
            "mesh": len(self.meshes) - 1,
            "extras": {
                "zengine_part": part.name,
                "zengine_kind": zboard.KIND_NAMES.get(part.kind, "unknown"),
                "zengine_material": part.material,
                "zengine_tag": part.tag,
                "lod_mask": part.lod_mask,
            },
        })

    def document(self, parts: Sequence[zboard.Part],
                 visual: Dict[str, Any]) -> bytes:
        board_nodes = []
        sky_nodes = []
        for node_id, part in enumerate(parts):
            (sky_nodes if part.kind == zboard.KIND_SKY
             else board_nodes).append(node_id)
        board_group = len(self.nodes)
        self.nodes.append({"name": "SceneMesh", "children": board_nodes})
        sky_group = len(self.nodes)
        self.nodes.append({"name": "Sky_Dome", "children": sky_nodes})
        root = len(self.nodes)
        self.nodes.append({"name": "Artist", "children": [board_group, sky_group]})

        doc = {
            "asset": {"version": "2.0",
                      "generator": "ZEngine zboard.py / blender_export.py"},
            "scene": 0,
            "scenes": [{"name": "ZEngine_AC90", "nodes": [root]}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            "buffers": [{"byteLength": len(self.binary)}],
            "extras": {
                "schema": BLENDER_SCHEMA,
                "hd_parts": len(parts),
                "visual_schema": visual["schema"],
                "visual_name": visual.get("name", ""),
                "coordinate_system": "glTF Y-up right-handed; Blender importer converts to Z-up",
                "source": "build/board.zbrd (ZBRD v1)",
            },
        }
        payload = json.dumps(doc, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        json_pad = b" " * (-len(payload) % 4)
        json_chunk = payload + json_pad
        bin_pad = b"\0" * (-len(self.binary) % 4)
        bin_chunk = bytes(self.binary) + bin_pad
        total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
        return b"".join((
            struct.pack("<3I", GLB_MAGIC, 2, total),
            struct.pack("<2I", len(json_chunk), CHUNK_JSON), json_chunk,
            struct.pack("<2I", len(bin_chunk), CHUNK_BIN), bin_chunk,
        ))


def build_glb(parts: Sequence[zboard.Part],
              visual: Dict[str, Any]) -> bytes:
    builder = GlbBuilder()
    for part in parts:
        builder.add_part(part, visual)
    return builder.document(parts, visual)


def parse_glb(data: bytes) -> Dict[str, Any]:
    """离线门禁用：校验 GLB 头、chunk 结构与 JSON 文档。"""
    if len(data) < 20:
        raise ValueError("GLB too short")
    magic, version, length = struct.unpack_from("<3I", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ValueError("bad GLB header")
    json_len, json_type = struct.unpack_from("<2I", data, 12)
    if json_type != CHUNK_JSON or 20 + json_len > len(data):
        raise ValueError("bad GLB JSON chunk")
    doc = json.loads(data[20:20 + json_len].decode("utf-8").rstrip(" "))
    bin_at = 20 + json_len
    bin_len, bin_type = struct.unpack_from("<2I", data, bin_at)
    if bin_type != CHUNK_BIN or bin_at + 8 + bin_len != len(data):
        raise ValueError("bad GLB BIN chunk")
    if doc["buffers"][0]["byteLength"] > bin_len:
        raise ValueError("buffer byteLength exceeds BIN chunk")

    component_size = {5121: 1, 5123: 2, 5125: 4, 5126: 4}
    dimensions = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
    for view in doc["bufferViews"]:
        if view["byteOffset"] + view["byteLength"] > bin_len:
            raise ValueError("bufferView exceeds BIN chunk")
    for accessor in doc["accessors"]:
        view = doc["bufferViews"][accessor["bufferView"]]
        needed = (accessor["count"] * dimensions[accessor["type"]]
                  * component_size[accessor["componentType"]])
        if needed > view["byteLength"]:
            raise ValueError("accessor exceeds its bufferView")
    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            for index in primitive["attributes"].values():
                if not 0 <= index < len(doc["accessors"]):
                    raise ValueError("attribute references an out-of-range accessor")
            if not 0 <= primitive["indices"] < len(doc["accessors"]):
                raise ValueError("indices reference an out-of-range accessor")
            if not 0 <= primitive["material"] < len(doc["materials"]):
                raise ValueError("primitive references an out-of-range material")
    for node in doc["nodes"]:
        if "mesh" in node and not 0 <= node["mesh"] < len(doc["meshes"]):
            raise ValueError("node references an out-of-range mesh")
        for child in node.get("children", []):
            if not 0 <= child < len(doc["nodes"]):
                raise ValueError("node references an out-of-range child")
    return doc


def visual_subset(visual: Dict[str, Any],
                  spec: Dict[str, Any]) -> Dict[str, Any]:
    """只把 bpy 脚本真正需要的 AI 参数内嵌进去，避免脚本依赖相对路径。"""
    return {
        "name": visual.get("name", ""),
        "schema": visual["schema"],
        "skybox": {
            "mode": visual["skybox"]["mode"],
            "exposure": visual["skybox"]["exposure"],
            "rotation_deg": visual["skybox"]["rotation_deg"],
            "top_color": visual["skybox"]["top_color"],
            "horizon_color": visual["skybox"]["horizon_color"],
            "bottom_color": visual["skybox"]["bottom_color"],
        },
        "post_processing": visual["post_processing"],
        "lighting": dict(visual["lighting"],
                         directional_euler=spec["lighting"]["directional_euler"]),
        "palette": visual["palette"],
    }


def _load_external() -> Dict[str, Any]:
    """读 build/external_mesh.manifest.json；没有就退化成「没有外部资产」。"""
    path = os.path.join(BUILD, "external_mesh.manifest.json")
    if not os.path.isfile(path):
        return {"schema": "", "coordinate_system": "", "assets": [],
                "placements": []}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def external_subset(manifest: Dict[str, Any]) -> Dict[str, Any]:
    """只把 asset_ready 的资产与实例嵌进 bpy 脚本，pending 条目保持沉默。"""
    return {
        "schema": manifest.get("schema", ""),
        "coordinate_system": manifest.get("coordinate_system", ""),
        "assets": [a for a in manifest.get("assets", [])
                   if a.get("status") == "asset_ready"],
        "placements": [p for p in manifest.get("placements", [])
                       if p.get("asset_ready")],
    }


def export_all(spec: Dict[str, Any], parts: Sequence[zboard.Part],
               visual: Dict[str, Any], out_dir: str = OUT,
               assets_dir: str = "") -> Dict[str, Any]:
    os.makedirs(out_dir, exist_ok=True)
    # 内存 Part 是 double，board.zbrd 里是 float32。统一先按 ZBRD 量化一次，
    # 否则「zboard --variants 直传内存」与「blender_export 读文件」会产出
    # 字节不同的 GLB，部署校验会误报 STALE。
    canonical = zboard.decode_zbrd(zboard.encode(parts))
    hd = [p for p in canonical if p.lod_mask & zboard.LOD_BITS["HD"]]
    glb = build_glb(hd, visual)
    with open(os.path.join(out_dir, GLB_NAME), "wb") as handle:
        handle.write(glb)
    # 外部单体资产与 bpy 脚本保持「包内相对路径」一致：build/blender/.. 与
    # trunk 的 Blender/.. 都是包根，脚本里只写 assets/external_meshes/<x>.glb。
    external = _load_external()
    for asset in external_subset(external)["assets"]:
        relative = asset["mesh"].replace("/", os.sep)
        source = os.path.join(ROOT, relative)
        destination = os.path.join(os.path.dirname(out_dir), relative)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copyfile(source, destination)
        with open(source, "rb") as handle:
            if hashlib.sha256(handle.read()).hexdigest() != asset["mesh_sha256"]:
                raise ValueError("external glb sha256 mismatch: " + asset["asset_id"])
        # baseColor 贴图与 glb 同路径 stage，脚本里的包内相对路径才成立。
        texture_relative = asset.get("texture")
        if texture_relative:
            t_source = os.path.join(ROOT, texture_relative.replace("/", os.sep))
            t_destination = os.path.join(os.path.dirname(out_dir),
                                         texture_relative.replace("/", os.sep))
            os.makedirs(os.path.dirname(t_destination), exist_ok=True)
            shutil.copyfile(t_source, t_destination)
    script = BPY_TEMPLATE.replace(
        "__ZENGINE_VISUAL_JSON__",
        json.dumps(visual_subset(visual, spec), ensure_ascii=False, indent=4)
    ).replace(
        "__ZENGINE_EXTERNAL_JSON__",
        json.dumps(external_subset(external), ensure_ascii=False, indent=4))
    with open(os.path.join(out_dir, SCRIPT_NAME), "w", encoding="utf-8",
              newline="\n") as handle:
        handle.write(script)
    source = os.path.join(assets_dir or ROOT, visual["skybox"]["panorama_image"])
    shutil.copyfile(source, os.path.join(out_dir, SKY_NAME))
    return verify_outputs(out_dir, visual, hd)


def _embedded_json(script: str, name: str) -> Any:
    """从 bpy 脚本里取回 `NAME = <json>` 常量（json 含 null/true/false）。"""
    for node in ast.parse(script).body:
        if (isinstance(node, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == name
                        for t in node.targets)):
            source = ast.get_source_segment(script, node.value)
            if source is None:
                break
            return json.loads(source)
    raise ValueError("bpy script has no `%s` table" % name)


def verify_outputs(out_dir: str, visual: Dict[str, Any],
                   parts: Sequence[zboard.Part]) -> Dict[str, Any]:
    with open(os.path.join(out_dir, GLB_NAME), "rb") as handle:
        doc = parse_glb(handle.read())
    with open(os.path.join(out_dir, SCRIPT_NAME), "r",
              encoding="utf-8") as handle:
        script = handle.read()
    with open(os.path.join(out_dir, SKY_NAME), "rb") as handle:
        sky_sha = hashlib.sha256(handle.read()).hexdigest()
    if sky_sha != visual["skybox"]["panorama_sha256"]:
        raise ValueError("Blender sky panorama does not match visual-pack sha256")
    if doc["extras"]["schema"] != BLENDER_SCHEMA:
        raise ValueError("unexpected GLB scene schema")
    if len(doc["meshes"]) != len(parts) or doc["extras"]["hd_parts"] != len(parts):
        raise ValueError("GLB mesh count does not match HD parts")
    names = [mesh["name"] for mesh in doc["meshes"]]
    if names != [part.name for part in parts]:
        raise ValueError("GLB mesh names drifted from ZBRD parts")
    for marker in ("import_scene.gltf", "ShaderNodeTexEnvironment",
                   "CompositorNodeGlare", "save_as_mainfile", BLEND_NAME,
                   "def import_external_meshes", "import_external_meshes()",
                   "hide_render = True", "proxy.display_type = \"WIRE\"",
                   "EXTERNAL_TEX_SKIP", "node.type == \"TEX_IMAGE\"",
                   "colorspace_settings.name = \"sRGB\""):
        if marker not in script:
            raise ValueError("build_ac90.py is missing `%s`" % marker)
    for placeholder in ("__ZENGINE_VISUAL_JSON__", "__ZENGINE_EXTERNAL_JSON__"):
        if placeholder in script:
            raise ValueError("bpy script placeholder %s was not injected"
                             % placeholder)
    # 外部单体资产：嵌入表必须与 build/external_mesh.manifest.json 一致，
    # 且包内相对路径的 glb 副本逐字节匹配 sha256。
    embedded = _embedded_json(script, "EXTERNAL")
    expected = external_subset(_load_external())
    if embedded != expected:
        raise ValueError("bpy script external table drifted from the manifest")
    for asset in expected["assets"]:
        relative = asset["mesh"].replace("/", os.sep)
        copied = os.path.join(os.path.dirname(out_dir), relative)
        if not os.path.isfile(copied):
            raise ValueError("external glb was not staged: " + asset["asset_id"])
        with open(copied, "rb") as handle:
            if hashlib.sha256(handle.read()).hexdigest() != asset["mesh_sha256"]:
                raise ValueError("staged external glb sha256 mismatch: "
                                 + asset["asset_id"])
        texture_relative = asset.get("texture")
        if texture_relative:
            tex_copied = os.path.join(os.path.dirname(out_dir),
                                      texture_relative.replace("/", os.sep))
            if not os.path.isfile(tex_copied):
                raise ValueError("external texture was not staged: "
                                 + asset["asset_id"])
            with open(tex_copied, "rb") as handle:
                if hashlib.sha256(handle.read()).hexdigest() != asset["texture_sha256"]:
                    raise ValueError("staged external texture sha256 mismatch: "
                                     + asset["asset_id"])
    return {"schema": BLENDER_SCHEMA, "hd_parts": len(parts),
            "materials": len(doc["materials"]), "nodes": len(doc["nodes"]),
            "glb_bytes": os.path.getsize(os.path.join(out_dir, GLB_NAME)),
            "sky_sha256": sky_sha, "blend_output": BLEND_NAME,
            "external_assets": len(expected["assets"]),
            "external_placements": len(expected["placements"])}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="导出 AC90 的 Blender 交付物")
    parser.add_argument("--spec", default=os.path.join(ROOT, "board-spec.json"))
    parser.add_argument("--zbrd", default=os.path.join(BUILD, "board.zbrd"))
    parser.add_argument("--visual", default=os.path.join(ROOT, "visual-pack.json"))
    parser.add_argument("--out", default=OUT)
    parser.add_argument("--check", action="store_true", help="只校验已有产物")
    args = parser.parse_args(argv)
    try:
        with open(args.spec, "r", encoding="utf-8") as handle:
            spec = json.load(handle)
        with open(args.visual, "r", encoding="utf-8") as handle:
            visual = json.load(handle)
        zboard.validate_visual_pack(visual, spec)
        with open(args.zbrd, "rb") as handle:
            parts = zboard.decode_zbrd(handle.read())
        if args.check:
            report = verify_outputs(args.out, visual,
                                    [p for p in parts
                                     if p.lod_mask & zboard.LOD_BITS["HD"]])
        else:
            report = export_all(spec, parts, visual, args.out)
        print("BLENDER OK: %s (%d HD parts / %d materials / %d nodes / %d bytes)"
              % (BLEND_NAME, report["hd_parts"], report["materials"],
                 report["nodes"], report["glb_bytes"]))
        return 0
    except (OSError, ValueError, zboard.ZBoardError) as exc:
        print("BLENDER FAILED: %s" % exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
