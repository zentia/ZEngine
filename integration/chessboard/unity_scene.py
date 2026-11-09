"""离线生成 Unity 高配棋盘场景的 `.unity` YAML，不调用任何 Unity Editor API。

为什么必须离线（2026-11-05 实测，连崩 3 次）：
OSGame 定制引擎 `Runtime/GfxDevice/GfxDevice.cpp:189-195` 在 GFX_TLS_DEVICE 下，
任何**非主线程**调用 `GetGfxDevice()` 都会 `FatalErrorMsg("Graphics device is null.")`；
`FatalErrorMsg` 想弹 DisplayDialog，但在非主线程被 `EditorUtility.cpp:1136-1140`
取消，于是 `Exiting without the bug reporter ... return code 0` —— 主动 exit(0)，
不是 native crash，所以没有 dump。菜单回调里做 AssetDB 导入 / NewScene 都会
触发这条路径（三次分别崩在 prefab 异步导入、资产导入和 step 3/5 之后的场景阶段）。

本模块只做纯文本 I/O：读已存在的 mesh/material 的 `.meta` GUID，写出一个
引用它们的 `.unity` 场景。用户无需在 Unity 里点任何生成菜单。

**场景级头部继承（默认，2026-11-05 起）**：`build/scene_template.unity` 缓存
一份现网 HD 场景全文，但**默认只取开头 4 个场景级文档**
（`OcclusionCullingSettings` / `RenderSettings` / `LightmapSettings` /
`NavMeshSettings`）作为 HEADER，棋盘对象仍全部由本模块自己写。这样补齐了
离线手写版本丢掉的 `m_LightmapEditorSettings`、`m_LightingDataAsset`、
`m_LightingSettings`，又不会把 AC10 的 31 个 PrefabInstance 带进来 ——
其中 11 个 `m_SourcePrefab` GUID 在 trunk 不存在，稳定 Editor 实测打开会刷
`Missing Prefab Asset` 并报 `Problem detected while opening the Scene file`。
**崩溃根因已确认是另一个 GPUParticle 定制引擎分支，与本场景结构无关**；
换 `G:/OSGame/Client_proj/UnityEngine` 的稳定 Editor 后 AC90 可正常加载。
`--full-template` 可完整继承（仅诊断用），`--no-template` 用内置 HEADER。

**AI 视觉层（visual-pack.json，2026-11-05 起）**：天空盒 / 后处理 / 材质 / Volume
四组参数也由 AI 生成，与冻结几何、外围装饰三者分离。本模块把它们落到
(1) `!u!104 RenderSettings` 的雾 / 环境光；(2) `RenderSettingGroup` 下新增的
`ZEngineVolume` —— 复用现网 AC10 的 `RenderSettingsVolume` MonoBehaviour 结构
（脚本 GUID 在 trunk 的 GameScripts Packages 里可解析），只覆写白名单参数并把
指向 AC10 场景对象的 fileID 清零；(3) `build/materials/*.mat` 的颜色补丁
（GUID / shader 原样保留）；(4) `sky_gradient.mat` 对 AI 全景贴图的绑定。
默认产出 89 文档 / 0 PrefabInstance。`--no-visual` 可退回纯几何场景。

用法：
  python unity_scene.py --assets G:/OSGame/.../Assets/ZEngineChessboard \
      --out build/ART_OS_AC90_HD.unity
  python unity_scene.py --template G:/OSGame/.../ART_OS_AC10_HD.unity  # 刷新母版
  python unity_scene.py --check     # 用缓存的 GUID 映射做结构自检
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
from typing import Any

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

import zboard  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402

ARTFIELD_SCRIPT_GUID = "603043be3babbf44188404cd589ac0c7"
ARTFIELD_SCRIPT_FILEID = -365591601
MESH_MAIN_FILEID = 4300000
MATERIAL_MAIN_FILEID = 2100000
BUILTIN_GUIDS = {"0000000000000000e000000000000000",
                 "0000000000000000f000000000000000"}
HD_BIT = 1
GUID_CACHE = "asset_guids.json"
TEMPLATE_CACHE = "scene_template.unity"
TEMPLATE_ID_BASE = 2000000000
# 现网场景里有 19 位 fileID（PrefabInstance / SceneRoots），必须给 ZEngine
# 区间加上界，否则会把母版文档误判成新增文档。
TEMPLATE_ID_END = TEMPLATE_ID_BASE + 100000
TEMPLATE_OLD_ARTIST_NAME = "Artist_AC10_Reference"
SCENE_LEVEL_CLASSES = {"29", "104", "157", "196"}
NULL_REF = "{fileID: 0}"
TEXTURE_MAIN_FILEID = 2800000
VOLUME_SCRIPT_FILEID = 11500000
VOLUME_NODE_NAME = "ZEngineVolume"
VISUAL_ASSET_DIR = "Textures"
VISUAL_IMAGES = ("sky_panorama.jpg", "concept.jpg")
# 外部单体装饰（glb + sha256 + 锚点）的 Unity 运行时消费端。
# 注意：trunk 的 GameScripts 是「外部编译成 Scripts.dll」再进
# Assets/Plugins/GameScripts 的，DLL 里的 MonoBehaviour 用的是
# Scripts.dll.meta 的 GUID + 类型 fileID。所以场景里**不能**写脚本文件
# 自己的 .meta GUID —— 那样必然 Missing (Mono Script)。这里只留一个
# 纯标记 GameObject；ZExternalMeshBinder 用 RuntimeInitializeOnLoadMethod
# 在运行时按名字找到它并接管。
EXTERNAL_NODE_NAME = "ExternalMeshRoot"
EXTERNAL_MANIFEST_NAME = "external_mesh.manifest.json"
EXTERNAL_ASSET_ROOT = "ZEngineChessboard"
LEGACY_BINDER_SCRIPT_GUID = "2391662872ad428a12356ebf07318402"
# GameScripts 编出来的 Scripts.dll 的 GUID。菜单「挂载外部网格消费端」之后，
# Unity 会把 m_Script 写成这个 GUID + 类型 fileID，校验必须放行。
SCRIPTS_DLL_GUID = "603043be3babbf44188404cd589ac0c7"

HEADER = """%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!29 &1
OcclusionCullingSettings:
  m_ObjectHideFlags: 0
  serializedVersion: 2
  m_OcclusionBakeSettings:
    smallestOccluder: 5
    smallestHole: 0.25
    backfaceThreshold: 100
  m_SceneGUID: 00000000000000000000000000000000
  m_OcclusionCullingData: {fileID: 0}
--- !u!104 &2
RenderSettings:
  m_ObjectHideFlags: 0
  serializedVersion: 9
  m_Fog: 0
  m_FogColor: {r: 0.5, g: 0.5, b: 0.5, a: 1}
  m_FogMode: 3
  m_FogDensity: 0.01
  m_LinearFogStart: 0
  m_LinearFogEnd: 300
  m_AmbientSkyColor: {r: 0.212, g: 0.227, b: 0.259, a: 1}
  m_AmbientEquatorColor: {r: 0.114, g: 0.125, b: 0.133, a: 1}
  m_AmbientGroundColor: {r: 0.047, g: 0.043, b: 0.035, a: 1}
  m_AmbientIntensity: 1
  m_AmbientMode: 0
  m_SubtractiveShadowColor: {r: 0.42, g: 0.478, b: 0.627, a: 1}
  m_SkyboxMaterial: {fileID: 0}
  m_HaloStrength: 0.5
  m_FlareStrength: 1
  m_FlareFadeSpeed: 3
  m_HaloTexture: {fileID: 0}
  m_SpotCookie: {fileID: 10001, guid: 0000000000000000e000000000000000, type: 0}
  m_DefaultReflectionMode: 1
  m_DefaultReflectionResolution: 128
  m_ReflectionBounces: 1
  m_ReflectionIntensity: 1
  m_CustomReflection: {fileID: 0}
  m_Sun: {fileID: 0}
  m_IndirectSpecularColor: {r: 0, g: 0, b: 0, a: 1}
  m_UseRadianceAmbientProbe: 0
--- !u!157 &3
LightmapSettings:
  m_ObjectHideFlags: 0
  serializedVersion: 12
  m_GIWorkflowMode: 1
  m_GISettings:
    serializedVersion: 2
    m_BounceScale: 1
    m_IndirectOutputScale: 1
    m_AlbedoBoost: 1
    m_EnvironmentLightingMode: 0
    m_EnableBakedLightmaps: 1
    m_EnableRealtimeLightmaps: 1
  m_LightingDataAsset: {fileID: 0}
  m_LightingSettings: {fileID: 0}
--- !u!196 &4
NavMeshSettings:
  serializedVersion: 2
  m_ObjectHideFlags: 0
  m_BuildSettings:
    serializedVersion: 3
    agentTypeID: 0
    agentRadius: 0.5
    agentHeight: 2
    agentSlope: 45
    agentClimb: 0.4
    ledgeDropHeight: 0
    maxJumpAcrossDistance: 0
    minRegionArea: 2
    manualCellSize: 0
    cellSize: 0.16666666
    manualTileSize: 0
    tileSize: 256
    buildHeightMesh: 0
    maxJobWorkers: 0
    preserveTilesOutsideBounds: 0
    debug:
      m_Flags: 0
  m_NavMeshData: {fileID: 0}
"""


def header_from_template(template: str) -> str:
    """只继承现网 HD 场景开头的 4 个场景级文档（含完整 GI / LightingSettings）。

    **默认模式**。2026-11-05 用稳定 Editor 实测：完整继承会带进 31 个
    PrefabInstance，其中 11 个 `m_SourcePrefab` GUID 在 trunk 里根本不存在，
    打开 AC90 时刷 9 条 `Prefab instance problem. Missing Prefab Asset` 并触发
    `Problem detected while opening the Scene file`；而随场景一起带进来的
    AC10 旧美术还会被 SceneExportChecker 收进打包产物。真正需要的只是
    `RenderSettings` / `LightmapSettings` 的场景级状态，其余全部由我们自己写。
    """
    head, *chunks = template.split("--- !u!")
    out = [head]
    found = []
    for chunk in chunks:
        class_id = chunk.split("\n", 1)[0].strip().split(" ", 1)[0]
        if class_id not in SCENE_LEVEL_CLASSES:
            break
        out.append(chunk)
        found.append(class_id)
    if set(found) != SCENE_LEVEL_CLASSES:
        raise ValueError("template must start with the 4 scene-level documents, "
                         "got %s" % found)
    result = "--- !u!".join(out)
    return result if result.endswith("\n") else result + "\n"


def apply_template(template: str, new_root: int) -> str:
    """以能正常打开的现网 HD 场景为母版，接入 ZEngine 的 Artist 根。

    为什么整份继承而不是补几个字段：AC90 是工程里唯一新增的 `.unity`，
    而现网 HD 场景能正常打开。它除了场景级 4 文档外还有 14 个根、
    31 个 PrefabInstance、445 个 MonoBehaviour 以及真实的
    `m_LightingDataAsset` / `m_LightingSettings`；离线手写场景把这些全丢了，
    打开时自定义 SRP/GI 走的是另一条初始化路径。母版保留全部 1494 个文档，
    只做三处最小改写：

    1. 原 `Artist` 根改名并禁用（不删文档）；
    2. `SceneRoots.m_Roots` 追加 ZEngine 新根；
    3. 末尾追加上层生成的棋盘文档。

    **不删 Artist 子树**：现网有 6 个 PrefabInstance 的 modifications 引用
    子树内 fileID（`254680459` / `351891918`），删掉会留下悬空引用。
    `SceneTools.cs` 按 `ART_NODE="Artist"` 取根，改名后仍只有我们的根叫 Artist。
    """
    declared = {int(v) for v in re.findall(
        r"^--- !u!\d+ &(-?\d+)\s*$", template, re.M)}
    if new_root in declared:
        raise ValueError("template already declares fileID %d" % new_root)
    head, *chunks = template.split("--- !u!")
    artist_docs = root_docs = 0
    out = [head]
    for chunk in chunks:
        first = chunk.split("\n", 1)[0].strip()
        match = re.match(r"(\d+) &(-?\d+)", first)
        class_id = int(match.group(1)) if match else -1
        if class_id == 1 and "  m_Name: Artist\n" in chunk:
            artist_docs += 1
            chunk = chunk.replace("  m_Name: Artist\n",
                                  "  m_Name: %s\n" % TEMPLATE_OLD_ARTIST_NAME, 1)
            chunk = chunk.replace("  m_IsActive: 1\n", "  m_IsActive: 0\n", 1)
        elif class_id == 1660057539:
            root_docs += 1
            lines = chunk.splitlines(keepends=True)
            idx = next((i for i, line in enumerate(lines)
                        if line.strip() == "m_Roots:"), -1)
            if idx < 0:
                raise ValueError("template SceneRoots has no m_Roots")
            cursor = idx + 1
            while cursor < len(lines) and lines[cursor].startswith("  - {fileID:"):
                cursor += 1
            lines.insert(cursor, "  - {fileID: %d}\n" % new_root)
            chunk = "".join(lines)
        out.append(chunk)
    if artist_docs != 1 or root_docs != 1:
        raise ValueError("template must have exactly 1 Artist GO and 1 SceneRoots, "
                         "got %d/%d" % (artist_docs, root_docs))
    result = "--- !u!".join(out)
    return result if result.endswith("\n") else result + "\n"


# --------------------------------------------------------------------------
# AI 视觉层：天空盒 / 后处理 / 材质 / Volume
# 全部是纯文本 I/O，不碰 Unity Editor API，也不新增未解析的脚本 GUID。
# --------------------------------------------------------------------------

def rgba(color: Any) -> str:
    values = [float(v) for v in color[:4]]
    return "{r: %.6f, g: %.6f, b: %.6f, a: %.6f}" % tuple(values)


def texture_guid(filename: str) -> str:
    """确定性 GUID：同一文件名永远得到同一个 GUID，避免每次生成漂移。"""
    key = "ZEngineChessboard/%s/%s" % (VISUAL_ASSET_DIR, filename)
    return hashlib.sha256(key.encode("utf-8")).hexdigest()[:32]


def replace_yaml_field(text: str, key: str, value: str,
                       required: bool = True) -> str:
    pattern = re.compile(r"^(\s*%s:\s*).*$" % re.escape(key), re.M)
    if not pattern.search(text):
        if required:
            raise ValueError("visual template is missing YAML field `%s`" % key)
        return text
    return pattern.sub(lambda m: m.group(1) + value, text)


def patch_render_settings(header: str, visual: dict[str, Any]) -> str:
    """把 AI 天空/灯光参数写进继承来的 !u!104 RenderSettings。"""
    fog = visual["skybox"]["fog"]
    light = visual["lighting"]
    ambient = light["ambient_color"]

    def scaled(factor: float) -> str:
        return "{r: %.6f, g: %.6f, b: %.6f, a: 1}" % tuple(
            float(ambient[i]) * factor for i in range(3))

    fields = (
        ("m_Fog", str(int(fog["enabled"]))),
        ("m_FogColor", rgba(fog["color"])),
        ("m_FogMode", str(int(fog["mode"]))),
        ("m_FogDensity", "%.6f" % float(fog["density"])),
        ("m_LinearFogStart", "%.6f" % float(fog["linear_start"])),
        ("m_LinearFogEnd", "%.6f" % float(fog["linear_end"])),
        ("m_AmbientSkyColor", rgba(ambient)),
        ("m_AmbientEquatorColor", scaled(0.7)),
        ("m_AmbientGroundColor", scaled(0.4)),
        ("m_AmbientIntensity", "%.6f" % float(light["ambient_intensity"])),
        ("m_SubtractiveShadowColor", rgba(light["subtractive_shadow_color"])),
    )
    head, *chunks = header.split("--- !u!")
    out = [head]
    found = False
    for chunk in chunks:
        if chunk.startswith("104 "):
            found = True
            for key, value in fields:
                chunk = replace_yaml_field(chunk, key, value)
        out.append(chunk)
    if not found:
        raise ValueError("visual pack needs a RenderSettings (!u!104) document")
    return "--- !u!".join(out)


def volume_document(template: str, document_id: str) -> str:
    """从现网 AC10 抽出 RenderSettingsVolume 的 MonoBehaviour 作为结构模板。"""
    for chunk in template.split("--- !u!")[1:]:
        if chunk.startswith(document_id + "\n"):
            return chunk.split("\n", 1)[1]
    raise ValueError("volume template document `%s` not found in stock scene"
                     % document_id)


def patch_volume_document(body: str, visual: dict[str, Any], go_id: int) -> str:
    """复用现网 Volume 结构，只覆写 AI 白名单参数，清零所有 AC10 外部引用。"""
    volume = visual["volume"]
    pp = visual["post_processing"]
    grading = pp["color_grading"]
    bloom = pp["bloom"]
    # m_Script 的形式是 {fileID: N, guid: ...}，不会被这条正则匹配。
    body = re.sub(r"\{fileID: (-?\d+)\}",
                  lambda m: m.group(0) if int(m.group(1)) == 0
                  else "{fileID: 0}", body)
    body = replace_yaml_field(body, "m_GameObject", "{fileID: %d}" % go_id)
    fields = (
        ("isGlobal", str(int(volume["is_global"]))),
        ("weight", "%.6f" % float(volume["weight"])),
        ("blendDistance", "%.6f" % float(volume["blend_distance"])),
        ("priority", str(int(volume["priority"]))),
        ("ExposureValue", "%.6f" % float(pp["exposure_value"])),
        ("TonemapMode", str(int(pp["tonemap_mode"]))),
        ("WhiteBalanceTemperature",
         "%.6f" % float(grading["white_balance_temperature"])),
        ("WhiteBalanceTint", "%.6f" % float(grading["white_balance_tint"])),
        ("GlobalSaturation", rgba(grading["global_saturation"])),
        ("GlobalContrast", rgba(grading["global_contrast"])),
        ("GlobalGamma", rgba(grading["global_gamma"])),
        ("GlobalGain", rgba(grading["global_gain"])),
        ("GlobalOffset", rgba(grading["global_offset"])),
        ("BlueCorrection", "%.6f" % float(grading["blue_correction"])),
        ("ExpandGamut", str(int(grading["expand_gamut"]))),
        ("ToneCurveType", str(int(grading["tone_curve_type"]))),
        ("LutIntensity", "%.6f" % float(pp["lut"]["intensity"])),
        ("EnableBloom", str(int(bloom["enabled"]))),
        ("BloomThreshold", "%.6f" % float(bloom["threshold"])),
        ("BloomSoftKnee", "%.6f" % float(bloom["soft_knee"])),
        ("BloomIntensity", "%.6f" % float(bloom["intensity"])),
        ("BloomDiffusion", "%.6f" % float(bloom["diffusion"])),
        ("BloomFireflyRemovalStrength",
         "%.6f" % float(bloom["firefly_removal_strength"])),
        ("BloomClamp", "%.6f" % float(bloom["clamp"])),
        ("BloomTint", rgba(bloom["tint"])),
    )
    for key, value in fields:
        body = replace_yaml_field(body, key, value)
    return body


def bind_texture_slot(text: str, slot: str, guid: str) -> str:
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.strip() != "- %s:" % slot:
            continue
        for j in range(i + 1, min(i + 5, len(lines))):
            if "m_Texture:" in lines[j]:
                lines[j] = re.sub(
                    r"\{fileID: 0\}",
                    "{fileID: %d, guid: %s, type: 3}"
                    % (TEXTURE_MAIN_FILEID, guid), lines[j], count=1)
                return "".join(lines)
    raise ValueError("material has no texture slot `%s`" % slot)


def patch_material(text: str, color: Any, sky_guid: str | None = None) -> str:
    """只覆写颜色属性；shader / GUID / 其余字段原样保留，避免 Missing。"""
    present = [key for key in ("_Color", "_BaseColor")
               if re.search(r"^\s*- %s:" % re.escape(key), text, re.M)]
    if not present:
        raise ValueError("material has none of the whitelisted color properties")
    for key in present:
        text = replace_yaml_field(text, "- " + key, rgba(color))
    if sky_guid is not None:
        for slot in ("_MainTex", "_AlbedoMap"):
            text = bind_texture_slot(text, slot, sky_guid)
    return text


def write_visual_assets(visual: dict[str, Any], assets_root: str,
                        build_root: str) -> list[str]:
    """生成 AI 材质补丁与贴图 .meta（产物，部署脚本逐字节比对）。"""
    mat_dir = os.path.join(assets_root, "Materials")
    out_mat = os.path.join(build_root, "materials")
    out_tex = os.path.join(build_root, "textures")
    os.makedirs(out_mat, exist_ok=True)
    os.makedirs(out_tex, exist_ok=True)
    sky_guid = texture_guid("sky_panorama.jpg")
    for name, entry in visual["materials"].items():
        source = os.path.join(mat_dir, name + ".mat")
        if not os.path.isfile(source):
            raise FileNotFoundError("missing material template %s" % source)
        with open(source, "r", encoding="utf-8") as handle:
            text = handle.read().replace("\r\n", "\n")
        text = patch_material(text, entry["color"],
                              sky_guid if name == "sky_gradient" else None)
        with open(os.path.join(out_mat, name + ".mat"), "w", encoding="utf-8",
                  newline="\n") as handle:
            handle.write(text)
    with open(os.path.join(ROOT, "assets", "texture_importer.meta.tpl"),
              "r", encoding="utf-8") as handle:
        meta_template = handle.read()
    for filename in VISUAL_IMAGES:
        meta = meta_template.replace("00000000000000000000000000000000",
                                     texture_guid(filename), 1)
        with open(os.path.join(out_tex, filename + ".meta"), "w",
                  encoding="utf-8", newline="\n") as handle:
            handle.write(meta)
    return sorted(visual["materials"])


def sanitize(value: str) -> str:
    """与 ZChessboardGenerator.cs 的 Sanitize() 保持一致。"""
    return "".join(c if (c.isalnum() and c.isascii()) or c == "_" else "_"
                   for c in value)


def quaternion_from_euler(euler: list[float]) -> tuple[float, ...]:
    cx, cy, cz = (math.radians(v) / 2 for v in euler)
    sx, sy, sz = math.sin(cx), math.sin(cy), math.sin(cz)
    c_x, c_y, c_z = math.cos(cx), math.cos(cy), math.cos(cz)
    return (sx * c_y * c_z + c_x * sy * sz,
            c_x * sy * c_z - sx * c_y * sz,
            c_x * c_y * sz + sx * sy * c_z,
            c_x * c_y * c_z - sx * sy * sz)


def read_guid(meta_path: str) -> str:
    with open(meta_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"^guid:\s*([0-9a-fA-F]{32})\s*$", text, re.M)
    if match is None:
        raise ValueError("no guid in %s" % meta_path)
    return match.group(1).lower()


class AssetGuids:
    def __init__(self) -> None:
        self.meshes: dict[str, str] = {}
        self.materials: dict[str, str] = {}

    @staticmethod
    def from_assets(assets_root: str, parts: list[zboard.Part]) -> "AssetGuids":
        result = AssetGuids()
        for part in parts:
            for path, key, table in (
                (os.path.join(assets_root, "Meshes",
                              sanitize(part.name) + ".asset.meta"),
                 part.name, result.meshes),
                (os.path.join(assets_root, "Materials",
                              sanitize(part.material) + ".mat.meta"),
                 part.material, result.materials),
            ):
                if key in table:
                    continue
                if not os.path.isfile(path):
                    raise FileNotFoundError(
                        "missing asset meta %s — the mesh/material assets must "
                        "already exist in the trunk" % path)
                table[key] = read_guid(path)
        return result

    @staticmethod
    def from_cache(path: str) -> "AssetGuids":
        with open(path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        result = AssetGuids()
        result.meshes = dict(raw["meshes"])
        result.materials = dict(raw["materials"])
        return result

    def to_json(self) -> dict[str, Any]:
        return {"schema": "zengine.chessboard.asset-guids/v1",
                "meshes": self.meshes, "materials": self.materials}

    def all_guids(self) -> set[str]:
        return set(self.meshes.values()) | set(self.materials.values())


class Node:
    def __init__(self, name: str, kind: str = "group",
                 part: zboard.Part | None = None,
                 payload: Any = None) -> None:
        self.name = name
        self.kind = kind            # group / part / artfield-host / light
        self.part = part
        self.payload = payload
        self.children: list[Node] = []
        self.go_id = 0
        self.t_id = 0
        self.comp_ids: list[int] = []
        self.rotation: tuple[float, ...] | None = None
        self.position: tuple[float, ...] | None = None

    def add(self, child: "Node") -> "Node":
        self.children.append(child)
        return self


class SceneBuilder:
    def __init__(self, id_base: int = 100000) -> None:
        self.next_id = id_base
        self.documents: list[str] = []
        self.ids: set[int] = set()
        self.names: list[str] = []
        self.guids = AssetGuids()
        self.volume_body = ""
        self.visual: dict[str, Any] | None = None

    def alloc(self) -> int:
        value = self.next_id
        self.next_id += 1
        self.ids.add(value)
        return value

    def allocate(self, node: Node) -> None:
        node.go_id = self.alloc()
        node.t_id = self.alloc()
        node.comp_ids = [node.t_id]
        if node.kind == "part":
            node.comp_ids.extend((self.alloc(), self.alloc()))
        elif node.kind in ("artfield-host", "light", "camera", "volume"):
            node.comp_ids.append(self.alloc())
        if node.kind == "light":
            node.rotation = quaternion_from_euler(node.payload[2])
        if node.kind == "camera":
            node.rotation = quaternion_from_euler(node.payload[0])
            node.position = node.payload[1]
        for child in node.children:
            self.allocate(child)

    def emit(self, node: Node, father_t: int) -> None:
        go_id, t_id = node.go_id, node.t_id
        if node.kind == "part":
            assert node.part is not None
            self.mesh_filter(node.comp_ids[1], go_id, self.guids.meshes[node.part.name])
            self.mesh_renderer(node.comp_ids[2], go_id,
                               self.guids.materials[node.part.material])
        elif node.kind == "artfield-host":
            self.art_field(node.comp_ids[1], go_id, go_id, node.payload)
        elif node.kind == "light":
            color, intensity, _euler = node.payload
            self.light(node.comp_ids[1], go_id, color, intensity)
        elif node.kind == "camera":
            self.camera(node.comp_ids[1], go_id)
        elif node.kind == "volume":
            assert self.visual is not None and self.volume_body
            body = patch_volume_document(self.volume_body, self.visual, go_id)
            self.add(114, node.comp_ids[1], body)
        self.gameobject(go_id, node.name, node.comp_ids)
        self.transform(t_id, go_id, father_t,
                       [child.t_id for child in node.children],
                       node.rotation, node.position)
        for child in node.children:
            self.emit(child, t_id)

    def add(self, class_id: int, object_id: int, body: str) -> None:
        self.documents.append("--- !u!%d &%d\n%s" % (class_id, object_id, body))

    def gameobject(self, go_id: int, name: str, comp_ids: list[int]) -> None:
        self.names.append(name)
        comps = "\n".join("  - component: {fileID: %d}" % cid for cid in comp_ids)
        self.add(1, go_id, "\n".join([
            "GameObject:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  serializedVersion: 6",
            "  m_Component:",
            comps,
            "  m_Layer: 0",
            "  m_Name: " + name,
            "  m_TagString: Untagged",
            "  m_Icon: " + NULL_REF,
            "  m_NavMeshLayer: 0",
            "  m_StaticEditorFlags: 0",
            "  m_IsActive: 1",
            "",
        ]))

    def transform(self, t_id: int, go_id: int, father_id: int,
                  children: list[int],
                  rotation: tuple[float, ...] | None = None,
                  position: tuple[float, ...] | None = None) -> None:
        rot = "{x: 0, y: 0, z: 0, w: 1}" if rotation is None else (
            "{x: %.8f, y: %.8f, z: %.8f, w: %.8f}" % rotation)
        pos = "{x: 0, y: 0, z: 0}" if position is None else (
            "{x: %.4f, y: %.4f, z: %.4f}" % position)
        child_block = ("  m_Children:\n"
                       + "\n".join("  - {fileID: %d}" % c for c in children)
                       if children else "  m_Children: []")
        self.add(4, t_id, "\n".join([
            "Transform:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  serializedVersion: 2",
            "  m_LocalRotation: " + rot,
            "  m_LocalPosition: " + pos,
            "  m_LocalScale: {x: 1, y: 1, z: 1}",
            "  m_ConstrainProportionsScale: 0",
            child_block,
            "  m_Father: {fileID: %d}" % father_id,
            "  m_LocalEulerAnglesHint: {x: 0, y: 0, z: 0}",
            "",
        ]))

    def mesh_filter(self, mf_id: int, go_id: int, guid: str) -> None:
        self.add(33, mf_id, "\n".join([
            "MeshFilter:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Mesh: {fileID: %d, guid: %s, type: 2}" % (MESH_MAIN_FILEID, guid),
            "",
        ]))

    def mesh_renderer(self, mr_id: int, go_id: int, guid: str) -> None:
        self.add(23, mr_id, "\n".join([
            "MeshRenderer:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Enabled: 1",
            "  m_CastShadows: 1",
            "  m_ReceiveShadows: 1",
            "  m_DynamicOccludee: 1",
            "  m_StaticShadowCaster: 0",
            "  m_MotionVectors: 1",
            "  m_LightProbeUsage: 1",
            "  m_ReflectionProbeUsage: 1",
            "  m_RayTracingMode: 2",
            "  m_RenderingLayerMask: 1",
            "  m_RendererPriority: 0",
            "  m_Materials:",
            "  - {fileID: %d, guid: %s, type: 2}" % (MATERIAL_MAIN_FILEID, guid),
            "  m_StaticBatchInfo:",
            "    firstSubMesh: 0",
            "    subMeshCount: 0",
            "  m_StaticBatchRoot: " + NULL_REF,
            "  m_ProbeAnchor: " + NULL_REF,
            "  m_LightProbeVolumeOverride: " + NULL_REF,
            "  m_ScaleInLightmap: 1",
            "  m_ReceiveGI: 1",
            "  m_PreserveUVs: 0",
            "  m_IgnoreNormalsForChartDetection: 0",
            "  m_ImportantGI: 0",
            "  m_StitchLightmapSeams: 1",
            "  m_SelectedEditorRenderState: 3",
            "  m_MinimumChartSize: 4",
            "  m_AutoUVMaxDistance: 0.5",
            "  m_AutoUVMaxAngle: 89",
            "  m_LightmapParameters: " + NULL_REF,
            "  m_SortingLayerID: 0",
            "  m_SortingLayer: 0",
            "  m_SortingOrder: 0",
            "  m_AdditionalVertexStreams: " + NULL_REF,
            "",
        ]))

    def light(self, light_id: int, go_id: int, color: list[float],
              intensity: float) -> None:
        self.add(108, light_id, "\n".join([
            "Light:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Enabled: 1",
            "  serializedVersion: 10",
            "  m_Type: 1",
            "  m_Shape: 0",
            "  m_Color: {r: %.6f, g: %.6f, b: %.6f, a: 1}"
            % (color[0], color[1], color[2]),
            "  m_Intensity: %.4f" % intensity,
            "  m_Range: 10",
            "  m_SpotAngle: 30",
            "  m_InnerSpotAngle: 30",
            "  m_CookieSize: 10",
            "  m_Shadows:",
            "    m_Type: 2",
            "    m_Resolution: -1",
            "    m_Strength: 1",
            "    m_Bias: 0.05",
            "    m_NormalBias: 0.4",
            "    m_NearPlane: 0.2",
            "    m_UseCullingMatrixOverride: 0",
            "  m_Cookie: " + NULL_REF,
            "  m_DrawHalo: 0",
            "  m_Flare: " + NULL_REF,
            "  m_RenderMode: 0",
            "  m_CullingMask:",
            "    serializedVersion: 2",
            "    m_Bits: 4294967295",
            "  m_RenderingLayerMask: 1",
            "  m_Lightmapping: 4",
            "  m_LightShadowCasterMode: 0",
            "  m_AreaSize: {x: 1, y: 1}",
            "  m_BounceIntensity: 1",
            "  m_ColorTemperature: 6500",
            "  m_UseColorTemperature: 0",
            "  m_BoundingSphereOverride: {x: 0, y: 0, z: 0, w: 0}",
            "  m_UseBoundingSphereOverride: 0",
            "  m_UseViewFrustumForShadowCasterCull: 1",
            "  m_ShadowRadius: 0",
            "  m_ShadowAngle: 0",
            "  m_BakeVisibility: 1",
            "",
        ]))

    def camera(self, camera_id: int, go_id: int) -> None:
        # 现网每套 HD 场景都有 1 个 !u!20 Camera；OSGame 的 YARP 自定义 SRP
        # 在无相机场景里可能走异常初始化路径（GfxDevice fatal）。
        self.add(20, camera_id, "\n".join([
            "Camera:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Enabled: 1",
            "  serializedVersion: 2",
            "  m_ClearFlags: 2",
            "  m_BackGroundColor: {r: 0.19215687, g: 0.3019608, b: 0.4745098, a: 0.019607844}",
            "  m_projectionMatrixMode: 1",
            "  m_GateFitMode: 2",
            "  m_FOVAxisMode: 0",
            "  m_SensorSize: {x: 36, y: 24}",
            "  m_LensShift: {x: 0, y: 0}",
            "  m_NormalizedViewPortRect:",
            "    serializedVersion: 2",
            "    x: 0",
            "    y: 0",
            "    width: 1",
            "    height: 1",
            "  near clip plane: 0.3",
            "  far clip plane: 1000",
            "  field of view: 31",
            "  orthographic: 0",
            "  orthographic size: 5",
            "  m_Depth: 0",
            "  m_CullingMask:",
            "    serializedVersion: 2",
            "    m_Bits: 4294967295",
            "  m_RenderingPath: -1",
            "  m_TargetTexture: " + NULL_REF,
            "  m_TargetDisplay: 0",
            "  m_TargetEye: 3",
            "  m_HDR: 0",
            "  m_AllowMSAA: 0",
            "  m_AllowDynamicResolution: 0",
            "  m_ForceIntoRT: 0",
            "  m_OcclusionCulling: 1",
            "",
        ]))

    def art_field(self, mb_id: int, go_id: int, field_go_id: int,
                  light_go_id: int) -> None:
        self.add(114, mb_id, "\n".join([
            "MonoBehaviour:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Enabled: 1",
            "  m_EditorHideFlags: 0",
            "  m_Script: {fileID: %d, guid: %s, type: 3}"
            % (ARTFIELD_SCRIPT_FILEID, ARTFIELD_SCRIPT_GUID),
            "  m_Name: ",
            "  m_EditorClassIdentifier: ",
            "  fieldRoot: {fileID: %d}" % field_go_id,
            "  directionalLight: {fileID: %d}" % light_go_id,
            "  openningAnim: " + NULL_REF,
            "  openningAnimTerrain: " + NULL_REF,
            "  sceneBackGround: " + NULL_REF,
            "  idleAnim: " + NULL_REF,
            "  ArraySceneAnim: []",
            "  sceneSlotOffset: {x: 0, y: 0.04, z: -0.06}",
            "  sceneBattleInfoHang: " + NULL_REF,
            "  sceneInteractcs: []",
            "  sceneTagComponents: []",
            "  ChangeStencil: 1",
            "",
        ]))

    def build(self, spec: dict[str, Any], info: dict[str, Any],
              parts: list[zboard.Part], guids: AssetGuids,
              template: str | None = None,
              full_template: bool = False,
              visual: dict[str, Any] | None = None,
              volume_source: str | None = None) -> str:
        self.guids = guids
        self.visual = visual
        selected = [p for p in parts if p.lod_mask & HD_BIT]
        scene = spec["scene"]
        lighting = spec["lighting"]

        board = Node("BoardMesh")
        sky = Node("Sky")
        for part in selected:
            target = sky if part.kind == zboard.KIND_SKY else board
            target.add(Node(part.name, "part", part=part))
        content = Node(sanitize(info["root_prefix"])).add(board).add(sky)

        light_go = Node("DirectionalLight", "light", payload=(
            list(lighting["directional_color"][:3]),
            float(lighting["directional_intensity"]),
            [float(v) for v in lighting["directional_euler"]]))
        light_group = Node("Light_Group").add(light_go)
        # 与现网 HD 场景同构：RenderSettingGroup 下挂一个 Main Camera。
        camera = Node("Main Camera", "camera",
                      payload=([20.0, 0.0, 0.0], (0.0, 10.0, -15.0)))
        render_group = Node("RenderSettingGroup").add(camera)
        if visual is not None and visual["volume"]["enabled"] and volume_source:
            self.volume_body = volume_document(
                volume_source, visual["volume"]["template_document"])
            render_group.add(Node(VOLUME_NODE_NAME, "volume"))
        field = Node(scene["field_node"], "artfield-host", payload=light_go.go_id)
        field.add(content)
        artist = Node(scene["artist_root"]).add(field).add(light_group)
        artist.add(render_group)
        external = info.get("external_mesh") or {}
        if int(external.get("consumed", 0)) > 0:
            artist.add(Node(EXTERNAL_NODE_NAME, "external"))

        self.allocate(artist)
        # light_go.go_id 在 allocate 之前是 0，分配后回填 ArtField 的引用。
        field.payload = light_go.go_id
        self.emit(artist, 0)
        if template is not None and full_template:
            full = apply_template(template, artist.t_id)
            if visual is not None:
                full = patch_render_settings(full, visual)
            return full + "\n".join(self.documents) + "\n"
        self.documents.append(
            "--- !u!1660057539 &9223372036854775807\n"
            "SceneRoots:\n"
            "  m_ObjectHideFlags: 0\n"
            "  m_Roots:\n"
            "  - {fileID: %d}\n" % artist.t_id)
        header = header_from_template(template) if template is not None else HEADER
        if visual is not None:
            header = patch_render_settings(header, visual)
        return header + "\n".join(self.documents)


def verify(text: str, spec: dict[str, Any], info: dict[str, Any],
           parts: list[zboard.Part], guids: AssetGuids) -> dict[str, Any]:
    """离线结构自检：fileID 引用闭合、GUID 可解析、层级/ArtField 契约正确。

    母版模式下只对 ZEngine 新增文档（fileID >= TEMPLATE_ID_BASE）做闭合与
    GUID 检查：现网母版自带 31 个 PrefabInstance，其 modifications 引用的是
    prefab 资产内部 fileID，本来就不在场景文档里声明。
    """
    template_mode = TEMPLATE_OLD_ARTIST_NAME in text
    chunks = text.split("--- !u!")[1:]
    declared = {int(v) for v in re.findall(r"^--- !u!\d+ &(-?\d+)\s*$",
                                           text, re.M)}
    if template_mode:
        ours = [chunk for chunk in chunks
                if (match := re.match(r"\d+ &(-?\d+)",
                                      chunk.split("\n", 1)[0].strip()))
                and TEMPLATE_ID_BASE <= int(match.group(1)) < TEMPLATE_ID_END]
        if not ours:
            raise ValueError("template scene has no ZEngine documents")
        scope = "--- !u!" + "--- !u!".join(ours)
        old_artist = [chunk for chunk in chunks
                      if "  m_Name: %s\n" % TEMPLATE_OLD_ARTIST_NAME in chunk]
        if len(old_artist) != 1 or "  m_IsActive: 0\n" not in old_artist[0]:
            raise ValueError("stock Artist must be renamed and disabled")
    else:
        # 继承来的场景级文档会引用现网的 LightingDataAsset / LightingSettings
        # GUID，它们不是棋盘资产，不参与 GUID 白名单校验。
        body = [chunk for chunk in chunks
                if chunk.split("\n", 1)[0].strip().split(" ", 1)[0]
                not in SCENE_LEVEL_CLASSES]
        scope = "--- !u!" + "--- !u!".join(body)

    visual_volume = info.get("visual", {}).get("volume", {})
    script_guid = visual_volume.get("script_guid")
    refs = [int(v) for v in re.findall(r"\{fileID: (-?\d+)\}", scope)]
    local_refs = [v for v in refs if v != 0 and v not in
                  (ARTFIELD_SCRIPT_FILEID, MESH_MAIN_FILEID,
                   MATERIAL_MAIN_FILEID, VOLUME_SCRIPT_FILEID, 10001, 15200)]
    dangling = sorted({v for v in local_refs if v not in declared})
    if dangling:
        raise ValueError("dangling local fileID refs: %s" % dangling[:8])

    used_guids = set(re.findall(r"guid: ([0-9a-fA-F]{32})", scope))
    known = guids.all_guids() | BUILTIN_GUIDS | {ARTFIELD_SCRIPT_GUID,
                                                 SCRIPTS_DLL_GUID}
    if script_guid:
        known.add(script_guid)
    unknown = sorted(used_guids - known)
    if unknown:
        raise ValueError("unknown GUID refs: %s" % unknown[:8])

    names = re.findall(r"^  m_Name: (.+)$", text, re.M)
    has_volume = VOLUME_NODE_NAME in names
    if has_volume:
        if not script_guid or script_guid not in scope:
            raise ValueError("AI Volume component is missing its script GUID")
        bloom_threshold = (info.get("visual", {}).get("post_processing", {})
                           .get("bloom", {}).get("threshold"))
        if (bloom_threshold is not None
                and not re.search(r"BloomThreshold: %.6f" % bloom_threshold, scope)):
            raise ValueError("AI Volume bloom parameters were not applied")
    has_external = EXTERNAL_NODE_NAME in names
    if has_external:
        # 纯标记节点：脚本由 GameScripts 外部编译进 Scripts.dll，
        # 运行时用 RuntimeInitializeOnLoadMethod 按名字接管。
        if LEGACY_BINDER_SCRIPT_GUID in scope:
            raise ValueError("ExternalMeshRoot must not reference a .cs.meta GUID "
                             "(GameScripts ships as Scripts.dll)")
    elif int((info.get("external_mesh") or {}).get("consumed", 0)) > 0:
        raise ValueError("asset_ready external meshes need an ExternalMeshRoot node")
    for expected in (spec["scene"]["artist_root"], spec["scene"]["field_node"],
                     "BoardMesh", "Sky", "Light_Group", "DirectionalLight",
                     "RenderSettingGroup", "Main Camera",
                     sanitize(info["root_prefix"])):
        if expected not in names:
            raise ValueError("missing scene node: %s" % expected)
    if names.count(spec["scene"]["artist_root"]) != 1:
        raise ValueError("scene must have exactly one root named Artist")
    if "--- !u!20 " not in text:
        raise ValueError("missing Camera (!u!20): every stock HD scene has one")
    selected = [p for p in parts if p.lod_mask & HD_BIT]
    for part in selected:
        if part.name not in names:
            raise ValueError("missing HD part node: %s" % part.name)
    mesh_refs = len(re.findall(r"^  m_Mesh: \{fileID: 4300000", scope, re.M))
    mat_refs = len(re.findall(r"^  - \{fileID: 2100000", scope, re.M))
    if mesh_refs != len(selected) or mat_refs != len(selected):
        raise ValueError("mesh/material refs %d/%d != %d HD parts"
                         % (mesh_refs, mat_refs, len(selected)))
    if "ChangeStencil: 1" not in scope or "fieldRoot: {fileID: 0}" in scope:
        raise ValueError("Project8ArtField contract broken")
    if "m_PrefabInstance: {fileID: 0}" not in scope:
        raise ValueError("prefab instance must be null (no prefab assets)")

    scene_roots = [chunk for chunk in chunks
                   if chunk.split("\n", 1)[0].strip().startswith("1660057539 ")]
    if len(scene_roots) != 1:
        raise ValueError("expected exactly one SceneRoots document")
    roots = [int(v) for v in re.findall(r"^  - \{fileID: (\d+)\}$",
                                        scene_roots[0], re.M)]
    if template_mode:
        new_roots = [v for v in roots
                     if TEMPLATE_ID_BASE <= v < TEMPLATE_ID_END]
        if len(new_roots) != 1:
            raise ValueError("template SceneRoots must contain exactly one "
                             "ZEngine root, got %d" % len(new_roots))
        root = new_roots[0]
    else:
        if len(roots) != 1:
            raise ValueError("expected exactly one SceneRoots entry, got %d"
                             % len(roots))
        root = roots[0]
    return {"documents": len(declared), "hd_parts": len(selected),
            "mesh_refs": mesh_refs, "material_refs": mat_refs,
            "guids": len(used_guids), "root": root,
            "template": template_mode, "volume": has_volume,
            "external": has_external,
            "visual": "visual" in info,
            "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest()}


def main(argv: list[str] | None = None) -> int:
    here = ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", default=os.path.join(here, "board-spec.json"))
    parser.add_argument("--manifest", default=os.path.join(here, "build", "board.manifest.json"))
    parser.add_argument("--zbrd", default=os.path.join(here, "build", "board.zbrd"))
    parser.add_argument("--out", default=os.path.join(here, "build", "ART_OS_AC90_HD.unity"))
    parser.add_argument("--guid-cache", default=os.path.join(here, "build", GUID_CACHE))
    parser.add_argument("--assets", default="", help="trunk Assets/ZEngineChessboard 目录")
    parser.add_argument("--template", default="",
                        help="现网 HD .unity 母版路径；传入则刷新本地缓存")
    parser.add_argument("--no-template", action="store_true",
                        help="退回内置 HEADER（不继承现网场景级状态）")
    parser.add_argument("--full-template", action="store_true",
                        help="完整继承现网场景全文（会带进旧美术与缺失 prefab，仅诊断用）")
    parser.add_argument("--visual", default=os.path.join(here, "visual-pack.json"),
                        help="AI 视觉包（天空盒 / 后处理 / 材质 / Volume）")
    parser.add_argument("--no-visual", action="store_true",
                        help="不应用视觉包：只生成几何场景")
    parser.add_argument("--check", action="store_true", help="只校验已生成的场景")
    args = parser.parse_args(argv)

    try:
        cache_path = os.path.join(here, "build", TEMPLATE_CACHE)
        cached_scene = ""
        if args.template:
            with open(args.template, "r", encoding="utf-8") as handle:
                cached_scene = handle.read().replace("\r\n", "\n")
            with open(cache_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(cached_scene)
        elif os.path.isfile(cache_path):
            with open(cache_path, "r", encoding="utf-8") as handle:
                cached_scene = handle.read()
        template = None if args.no_template else (cached_scene or None)
        with open(args.spec, "r", encoding="utf-8") as handle:
            spec = json.load(handle)
        with open(args.manifest, "r", encoding="utf-8") as handle:
            info = json.load(handle)
        with open(args.zbrd, "rb") as handle:
            parts = zboard.decode_zbrd(handle.read())
        guids = AssetGuids.from_cache(args.guid_cache)
        if args.assets and not args.check:
            hd_parts = [p for p in parts if p.lod_mask & HD_BIT]
            guids = AssetGuids.from_assets(args.assets, hd_parts)
            with open(args.guid_cache, "w", encoding="utf-8") as handle:
                json.dump(guids.to_json(), handle, ensure_ascii=False, indent=2)
        visual = None
        if not args.no_visual:
            visual = zboard.load_visual_pack(args.visual, spec)
            zboard.validate_visual_materials(visual, sorted(guids.materials))
        if args.check:
            with open(args.out, "r", encoding="utf-8") as handle:
                text = handle.read()
        else:
            full = template is not None and args.full_template
            builder = SceneBuilder(TEMPLATE_ID_BASE if full else 100000)
            text = builder.build(spec, info, parts, guids, template, full,
                                 visual=visual,
                                 volume_source=cached_scene or None)
            with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
            if visual is not None:
                assets_root = args.assets or os.path.join(
                    spec["target"]["project_root"], "Assets", "ZEngineChessboard")
                write_visual_assets(visual, assets_root,
                                    os.path.join(here, "build"))
        report = verify(text, spec, info, parts, guids)
        print("SCENE OK: %s (%d documents, %d HD parts, sha256=%s)"
              % (os.path.basename(args.out), report["documents"],
                 report["hd_parts"], report["sha256"][:16]))
        return 0
    except Exception as exception:  # noqa: BLE001
        print("SCENE FAILED: %s" % exception, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
