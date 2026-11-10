"""restore-profile 驱动的「拼现有资产出场景」：把 glb 烘焙成 .asset Mesh，
再按 manifest 摆放生成引用它们的 .unity 场景，全程离线、零 Unity Editor 依赖。

设计（2026-11-09）
----------------
用户最终选择「先拼现有资产出场景 + 直接烘焙 glb 进场景」。与
`unity_scene.py`（冻结棋盘几何 + 视觉层）不同，本模块只做**外部单体装饰**
（star_pillar / halo_ring / moon_orrery / star_lantern 这 4 个混元 glb）：

1. **烘焙 glb -> .asset Mesh**（明文、非压缩）：读 glb 的 POSITION/NORMAL/
   TEXCOORD_0 + indices（复用 `external_mesh.glb_indexed`），写成 Unity 原生
   `!u!43 Mesh` 的 `m_VertexData`（m_MeshCompression: 0），顶点保持**局部坐标**
   （glb 原始局部系，不含 manifest 的 anchor/rotation/scale）。摆放交给
   Transform，这样一份 Mesh 资产可被多个 placement 复用 —— 符合「可复用资产」
   的正规做法（现有现网 decoration_*.asset 是把世界坐标 bake 进顶点，本模块
   改走局部网格 + Transform 摆放，更干净）。

2. **贴图**：复用 `external_mesh.glb_embedded_texture` 抽出 glb 内嵌 PNG，落成
   `<asset_id>.png`（与 manifest 里的 texture_sha256 锁定同一份字节）。

3. **材质**：不新建材质，引用现网已有的 `zboard_*.mat`（GUID 从
   `build/asset_guids.json` 读）。每个 placement 的 `material` 字段已指向它们。

4. **场景**：每个 placement 生成一个 GameObject（Transform 带 anchor/rotation_y_deg/
   scale），MeshFilter 引用烘焙的 .asset，MeshRenderer 引用对应 .mat。头部沿用
   `build/scene_template.unity` 的 4 个场景级文档（与 unity_scene.py 同口径），
   并落一个 Main Camera（restore-profile 的 camera 参数）。

坐标契约
--------
    world = anchor + rotY(rotation_y_deg) * (scale * (local - pivot_offset))

本模块把 `-pivot_offset` 烘进 Mesh 顶点（局部系原点对齐），`anchor/rotation/scale`
交给 Transform。即：写出顶点 = scale * (local - pivot_offset) 中的
(local - pivot_offset) 部分由 Transform 的 scale 之外的平移吸收 —— 实际实现为
把 (local - pivot_offset) 写成顶点，Transform.m_LocalPosition = anchor、
m_LocalRotation = rotY、m_LocalScale = scale。这样等价且资产可复用。

用法
----
    python restore_scene.py --profile restore-profile.example.json \
        --manifest build/external_mesh.manifest.json \
        --guids build/asset_guids.json \
        --out build/restored_scenes/<id>.unity \
        --asset-dir build/restored_assets
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
from typing import Any, Dict, List, Sequence, Tuple

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

import external_mesh  # noqa: E402

MESH_MAIN_FILEID = 4300000
MATERIAL_MAIN_FILEID = 2100000
TEXTURE_MAIN_FILEID = 2800000
NULL_REF = "{fileID: 0}"

# 场景级文档类（与 unity_scene.py 同口径）
SCENE_LEVEL_CLASSES = {"29", "104", "157", "196"}

# Blender 可执行文件（OSGame 自编译引擎环境里的 headless 转换器）。
# 用于 glb -> FBX（原始资产，与现网 235 个 FBX 同一管线）。
BLENDER_EXE = os.environ.get(
    "ZENGINE_BLENDER",
    "D:/build_windows_x64_vc18_Release/bin/Release/blender.exe")
GLB_TO_FBX_SCRIPT = os.path.join(ROOT, "glb_to_fbx.py")


# --------------------------------------------------------------------------
# 数值格式化
# --------------------------------------------------------------------------

def _f(value: float) -> str:
    """顶点/法线/UV 用足够精度，避免 YAML 里丢精度导致渲染破面。"""
    if value == 0.0:
        return "0"
    return "%.8g" % value


def _fmt_vec3(values: Sequence[float]) -> str:
    return "{x: %s, y: %s, z: %s}" % (_f(values[0]), _f(values[1]), _f(values[2]))


def _fmt_vec2(values: Sequence[float]) -> str:
    return "{x: %s, y: %s}" % (_f(values[0]), _f(values[1]))


def _fmt_vec4(values: Sequence[float]) -> str:
    return "{x: %s, y: %s, z: %s, w: %s}" % (
        _f(values[0]), _f(values[1]), _f(values[2]), _f(values[3]))


# --------------------------------------------------------------------------
# glb -> .asset Mesh（明文、非压缩）
# --------------------------------------------------------------------------

def _channel(stream: int, offset: int, fmt: int, dimension: int) -> str:
    return ("    - stream: %d\n      offset: %d\n      format: %d\n"
            "      dimension: %d" % (stream, offset, fmt, dimension))


def mesh_asset_body(name: str, positions: Sequence[float],
                    normals: Sequence[float], uvs: Sequence[float],
                    indices: Sequence[int]) -> str:
    """生成 `!u!43 Mesh` 文档体（不含 `--- !u!43 &id` 头）。

    Unity 明文 Mesh 的顶点布局是**固定 64 字节 stride、8 个 VertexAttribute
    槽位**（2026-11-09 从现网 `SM_TV_006@N2C.asset` 反推并逐字段核对）：

        float32 偏移  属性        m_Channels 槽位
        [0:3]    position  offset 0,  dim 3   (channel 0)
        [3:6]    normal    offset 12, dim 3   (channel 1)
        [6:10]   color     offset 24, dim 4   (channel 2)
        [10:12]  uv0       offset 40, dim 2   (channel 4)
        [12:16]  tangent   offset 48, dim 4   (channel 6)

    m_Channels 固定 14 行（Unity 内部 VertexAttribute 数组），channel 3/5/7/8-13
    为 dimension:0 占位。索引独立写入 m_IndexBuffer（uint16，顶点数<=65535）。
    """
    vertex_count = len(positions) // 3
    index_count = len(indices)

    # 顶点交错二进制：每顶点 64 字节 = 16 float32
    #   [px,py,pz, nx,ny,nz, r,g,b,a, u,v, tx,ty,tz,tw]
    import struct
    vertex_blob = bytearray()
    for i in range(vertex_count):
        px, py, pz = positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]
        nx, ny, nz = normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]
        u, v = uvs[i * 2], uvs[i * 2 + 1]
        # color 用白色 opaque；tangent 用 (1,0,0,1)（Unity 默认，可被重建）
        vertex_blob += struct.pack(
            "<16f",
            px, py, pz,          # position (0:3)
            nx, ny, nz,          # normal   (3:6)
            1.0, 1.0, 1.0, 1.0,  # color    (6:10)
            u, v,                # uv0      (10:12)
            1.0, 0.0, 0.0, 1.0,  # tangent  (12:16)
        )

    # 索引二进制：uint16（index_format 0）；超 65535 顶点才需要 uint32。
    index_format = 0 if vertex_count <= 65535 else 1
    index_blob = bytearray()
    if index_format == 0:
        for idx in indices:
            index_blob += struct.pack("<H", idx)
    else:
        for idx in indices:
            index_blob += struct.pack("<I", idx)

    # localAABB（顶点局部坐标的包围盒）
    lo = [min(positions[a::3]) for a in range(3)]
    hi = [max(positions[a::3]) for a in range(3)]
    center = [(lo[a] + hi[a]) / 2 for a in range(3)]
    extent = [(hi[a] - lo[a]) / 2 for a in range(3)]

    stride = 64
    lines = [
        "Mesh:",
        "  m_ObjectHideFlags: 0",
        "  m_CorrespondingSourceObject: " + NULL_REF,
        "  m_PrefabInstance: " + NULL_REF,
        "  m_PrefabAsset: " + NULL_REF,
        "  m_Name: " + name,
        "  serializedVersion: 12",
        "  m_ForceKeepVerticesAndIndicesOnBuild: 0",
        "  m_SubMeshes:",
        "  - serializedVersion: 2",
        "    firstByte: 0",
        "    indexCount: %d" % index_count,
        "    topology: 0",
        "    baseVertex: 0",
        "    firstVertex: 0",
        "    vertexCount: %d" % vertex_count,
        "    localAABB:",
        "      m_Center: %s" % _fmt_vec3(center),
        "      m_Extent: %s" % _fmt_vec3(extent),
        "  m_Shapes:",
        "    vertices: []",
        "    shapes: []",
        "    channels: []",
        "    fullWeights: []",
        "  m_BindPose: []",
        "  m_BoneNameHashes: ",
        "  m_RootBoneNameHash: 0",
        "  m_BonesAABB: []",
        "  m_VariableBoneCountWeights:",
        "    m_Data: ",
        "  m_MeshCompression: 0",
        "  m_IsReadable: 1",
        "  m_KeepVertices: 1",
        "  m_KeepIndices: 1",
        "  m_IndexFormat: %d" % index_format,
        "  m_IndexBuffer: %s" % index_blob.hex(),
        "  m_VertexData:",
        "    serializedVersion: 3",
        "    m_VertexCount: %d" % vertex_count,
        "    m_Channels:",
        _channel(0, 0, 0, 3),    # position
        _channel(0, 12, 0, 3),   # normal
        _channel(0, 24, 0, 4),   # color
        _channel(0, 0, 0, 0),    # (占位)
        _channel(0, 40, 0, 2),   # uv0
        _channel(0, 0, 0, 0),    # (占位)
        _channel(0, 48, 0, 4),   # tangent
        _channel(0, 0, 0, 0),    # (占位)
        _channel(0, 0, 0, 0),
        _channel(0, 0, 0, 0),
        _channel(0, 0, 0, 0),
        _channel(0, 0, 0, 0),
        _channel(0, 0, 0, 0),
        _channel(0, 0, 0, 0),
        "    m_DataSize: %d" % (vertex_count * stride),
        "    _typelessdata: %s" % vertex_blob.hex(),
        "  m_CompressedMesh:",
        "    m_Vertices:",
        "      m_NumItems: 0",
        "      m_Range: 0",
        "      m_Start: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_UV:",
        "      m_NumItems: 0",
        "      m_Range: 0",
        "      m_Start: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_Normals:",
        "      m_NumItems: 0",
        "      m_Range: 0",
        "      m_Start: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_Tangents:",
        "      m_NumItems: 0",
        "      m_Range: 0",
        "      m_Start: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_Weights:",
        "      m_NumItems: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_NormalSigns:",
        "      m_NumItems: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_TangentSigns:",
        "      m_NumItems: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_FloatColors:",
        "      m_NumItems: 0",
        "      m_Range: 0",
        "      m_Start: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_BoneIndices:",
        "      m_NumItems: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_Triangles:",
        "      m_NumItems: 0",
        "      m_Data: ",
        "      m_BitSize: 0",
        "    m_UVInfo: 0",
        "  m_LocalAABB:",
        "    m_Center: %s" % _fmt_vec3(center),
        "    m_Extent: %s" % _fmt_vec3(extent),
        "  m_MeshUsageFlags: 0",
        "  m_CookingOptions: 30",
        "  m_BakedConvexCollisionMesh: ",
        "  m_BakedTriangleCollisionMesh: ",
        "  m_MeshMetrics[0]: 1",
        "  m_MeshMetrics[1]: 1",
        "  m_MeshOptimizationFlags: -1",
        "  m_StreamData:",
        "    serializedVersion: 2",
        "    offset: 0",
        "    size: 0",
        "    path: ",
        "",
    ]
    return "\n".join(lines)


def write_mesh_asset(path: str, name: str, positions: Sequence[float],
                     normals: Sequence[float], uvs: Sequence[float],
                     indices: Sequence[int]) -> None:
    body = mesh_asset_body(name, positions, normals, uvs, indices)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n")
        handle.write("--- !u!43 &%d\n" % MESH_MAIN_FILEID)
        handle.write(body)


def write_mesh_meta(path: str, guid: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "fileFormatVersion: 2\n"
            "guid: %s\n"
            "NativeFormatImporter:\n"
            "  externalObjects: {}\n"
            "  mainObjectFileID: %d\n"
            "  userData: \n"
            "  assetBundleName: \n"
            "  assetBundleVariant: \n" % (guid, MESH_MAIN_FILEID))


def write_texture_meta(path: str, guid: str) -> None:
    """写 TextureImporter 的 .meta（逐字段对齐现网 `concept.jpg.meta`）。

    注意：必须用 `% guid` 替换 guid 占位符，否则写出字面量 `guid: %s` 会被
    Unity 判为 invalid GUID（2026-11-09 实测报错）。贴图默认 textureType:0
    （Default）、spriteMode:0，与现网普通贴图一致。
    """
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "fileFormatVersion: 2\n"
            "guid: %s\n"
            "TextureImporter:\n"
            "  internalIDToNameTable:\n"
            "  - first:\n"
            "      89: 8900000\n"
            "    second: generatedCubemap\n"
            "  externalObjects: {}\n"
            "  serializedVersion: 12\n"
            "  mipmaps:\n"
            "    mipMapMode: 0\n"
            "    enableMipMap: 0\n"
            "    sRGBTexture: 1\n"
            "    linearTexture: 0\n"
            "    fadeOut: 0\n"
            "    borderMipMap: 0\n"
            "    mipMapsPreserveCoverage: 0\n"
            "    alphaTestReferenceValue: 0.5\n"
            "    mipMapFadeDistanceStart: 1\n"
            "    mipMapFadeDistanceEnd: 3\n"
            "  bumpmap:\n"
            "    convertToNormalMap: 0\n"
            "    externalNormalMap: 0\n"
            "    heightScale: 0.25\n"
            "    normalMapFilter: 0\n"
            "    flipGreenChannel: 0\n"
            "  isReadable: 0\n"
            "  streamingMipmaps: 0\n"
            "  streamingMipmapsPriority: 0\n"
            "  vTOnly: 0\n"
            "  ignoreMipmapLimit: 0\n"
            "  grayScaleToAlpha: 0\n"
            "  generateCubemap: 6\n"
            "  cubemapConvolution: 0\n"
            "  seamlessCubemap: 0\n"
            "  textureFormat: 1\n"
            "  maxTextureSize: 2048\n"
            "  textureSettings:\n"
            "    serializedVersion: 2\n"
            "    filterMode: 1\n"
            "    aniso: 1\n"
            "    mipBias: 0\n"
            "    wrapU: 0\n"
            "    wrapV: 0\n"
            "    wrapW: 0\n"
            "  nPOTScale: 1\n"
            "  lightmap: 0\n"
            "  compressionQuality: 50\n"
            "  spriteMode: 0\n"
            "  spriteExtrude: 1\n"
            "  spriteMeshType: 1\n"
            "  alignment: 0\n"
            "  spritePivot: {x: 0.5, y: 0.5}\n"
            "  spritePixelsToUnits: 100\n"
            "  spriteBorder: {x: 0, y: 0, z: 0, w: 0}\n"
            "  spriteGenerateFallbackPhysicsShape: 1\n"
            "  alphaUsage: 1\n"
            "  alphaIsTransparency: 0\n"
            "  spriteTessellationDetail: -1\n"
            "  textureType: 0\n"
            "  textureShape: 1\n"
            "  singleChannelComponent: 0\n"
            "  flipbookRows: 1\n"
            "  flipbookColumns: 1\n"
            "  maxTextureSizeSet: 0\n"
            "  compressionQualitySet: 0\n"
            "  textureFormatSet: 0\n"
            "  ignorePngGamma: 0\n"
            "  applyGammaDecoding: 1\n"
            "  swizzle: 50462976\n"
            "  cookieLightType: 1\n"
            "  platformSettings:\n"
            "  - serializedVersion: 3\n"
            "    buildTarget: DefaultTexturePlatform\n"
            "    maxTextureSize: 2048\n"
            "    resizeAlgorithm: 0\n"
            "    textureFormat: -1\n"
            "    textureCompression: 1\n"
            "    compressionQuality: 50\n"
            "    crunchedCompression: 0\n"
            "    allowsAlphaSplitting: 0\n"
            "    overridden: 0\n"
            "    ignorePlatformSupport: 0\n"
            "    androidETC2FallbackOverride: 0\n"
            "    forceMaximumCompressionQuality_BC6H_BC7: 0\n"
            "  - serializedVersion: 3\n"
            "    buildTarget: iPhone\n"
            "    maxTextureSize: 2048\n"
            "    resizeAlgorithm: 0\n"
            "    textureFormat: 50\n"
            "    textureCompression: 1\n"
            "    compressionQuality: 50\n"
            "    crunchedCompression: 0\n"
            "    allowsAlphaSplitting: 0\n"
            "    overridden: 1\n"
            "    ignorePlatformSupport: 0\n"
            "    androidETC2FallbackOverride: 0\n"
            "    forceMaximumCompressionQuality_BC6H_BC7: 0\n"
            "  - serializedVersion: 3\n"
            "    buildTarget: Android\n"
            "    maxTextureSize: 2048\n"
            "    resizeAlgorithm: 0\n"
            "    textureFormat: 50\n"
            "    textureCompression: 1\n"
            "    compressionQuality: 50\n"
            "    crunchedCompression: 0\n"
            "    allowsAlphaSplitting: 0\n"
            "    overridden: 1\n"
            "    ignorePlatformSupport: 0\n"
            "    androidETC2FallbackOverride: 0\n"
            "    forceMaximumCompressionQuality_BC6H_BC7: 0\n"
            "  - serializedVersion: 3\n"
            "    buildTarget: Standalone\n"
            "    maxTextureSize: 2048\n"
            "    resizeAlgorithm: 0\n"
            "    textureFormat: 25\n"
            "    textureCompression: 1\n"
            "    compressionQuality: 50\n"
            "    crunchedCompression: 0\n"
            "    allowsAlphaSplitting: 0\n"
            "    overridden: 1\n"
            "    ignorePlatformSupport: 0\n"
            "    androidETC2FallbackOverride: 0\n"
            "    forceMaximumCompressionQuality_BC6H_BC7: 0\n"
            "  - serializedVersion: 3\n"
            "    buildTarget: OpenHarmony\n"
            "    maxTextureSize: 2048\n"
            "    resizeAlgorithm: 0\n"
            "    textureFormat: 50\n"
            "    textureCompression: 1\n"
            "    compressionQuality: 50\n"
            "    crunchedCompression: 0\n"
            "    allowsAlphaSplitting: 0\n"
            "    overridden: 1\n"
            "    ignorePlatformSupport: 0\n"
            "    androidETC2FallbackOverride: 0\n"
            "    forceMaximumCompressionQuality_BC6H_BC7: 0\n"
            "  spriteSheet:\n"
            "    serializedVersion: 2\n"
            "    sprites: []\n"
            "    outline: []\n"
            "    physicsShape: []\n"
            "    bones: []\n"
            "    spriteID: \n"
            "    internalID: 0\n"
            "    vertices: []\n"
            "    indices: \n"
            "    edges: []\n"
            "    weights: []\n"
            "    secondaryTextures: []\n"
            "    nameFileIdTable: {}\n"
            "  mipmapLimitGroupName: \n"
            "  pSDRemoveMatte: 0\n"
            "  userData: \n"
            "  assetBundleName: \n"
            "  assetBundleVariant: \n" % guid)


# --------------------------------------------------------------------------
# glb -> FBX（原始资产，与现网 235 个 FBX 同一管线）
# --------------------------------------------------------------------------

def write_fbx_meta(path: str, guid: str) -> None:
    """写 ModelImporter 的 .meta（对齐现网 `lubanqiyao_xiantiao3.fbx.meta`）。

    关键字段与现网一致：`meshes.fileIdsGeneration: 2`（Unity 按内容哈希生成
    mesh fileID）、`bakeAxisConversion: 0`、`useFileScale: 1`、`weldVertices: 1`、
    `tangentSpace.tangentImportMode: 3`（Unity 导入时算切线）。FBX 是美术源
    资产，Unity 导入后 mesh 以 `type:3`（sub-asset）+ `fileID:4300000` 被引用。
    """
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "fileFormatVersion: 2\n"
            "guid: %s\n"
            "ModelImporter:\n"
            "  serializedVersion: 22200\n"
            "  internalIDToNameTable: []\n"
            "  externalObjects: {}\n"
            "  materials:\n"
            "    materialImportMode: 0\n"
            "    materialName: 0\n"
            "    materialSearch: 1\n"
            "    materialLocation: 1\n"
            "  animations:\n"
            "    legacyGenerateAnimations: 4\n"
            "    bakeSimulation: 0\n"
            "    resampleCurves: 1\n"
            "    optimizeGameObjects: 0\n"
            "    removeConstantScaleCurves: 0\n"
            "    motionNodeName: \n"
            "    rigImportErrors: \n"
            "    rigImportWarnings: \n"
            "    animationImportErrors: \n"
            "    animationImportWarnings: \n"
            "    animationRetargetingWarnings: \n"
            "    animationDoRetargetingWarnings: 0\n"
            "    importAnimatedCustomProperties: 0\n"
            "    importConstraints: 0\n"
            "    animationCompression: 1\n"
            "    animationRotationError: 0.5\n"
            "    animationPositionError: 0.5\n"
            "    animationScaleError: 0.5\n"
            "    animationWrapMode: 0\n"
            "    extraExposedTransformPaths: []\n"
            "    extraUserProperties: []\n"
            "    clipAnimations: []\n"
            "    isReadable: 0\n"
            "  meshes:\n"
            "    lODScreenPercentages: []\n"
            "    globalScale: 1\n"
            "    meshCompression: 0\n"
            "    addColliders: 0\n"
            "    useSRGBMaterialColor: 1\n"
            "    sortHierarchyByName: 1\n"
            "    importPhysicalCameras: 1\n"
            "    importVisibility: 1\n"
            "    importBlendShapes: 1\n"
            "    importCameras: 1\n"
            "    importLights: 1\n"
            "    nodeNameCollisionStrategy: 1\n"
            "    fileIdsGeneration: 2\n"
            "    swapUVChannels: 0\n"
            "    generateSecondaryUV: 0\n"
            "    useFileUnits: 1\n"
            "    keepQuads: 0\n"
            "    weldVertices: 1\n"
            "    bakeAxisConversion: 0\n"
            "    preserveHierarchy: 0\n"
            "    skinWeightsMode: 0\n"
            "    maxBonesPerVertex: 4\n"
            "    minBoneWeight: 0.001\n"
            "    optimizeBones: 1\n"
            "    meshOptimizationFlags: -1\n"
            "    indexFormat: 0\n"
            "    secondaryUVAngleDistortion: 8\n"
            "    secondaryUVAreaDistortion: 15.000001\n"
            "    secondaryUVHardAngle: 88\n"
            "    secondaryUVMarginMethod: 1\n"
            "    secondaryUVMinLightmapResolution: 40\n"
            "    secondaryUVMinObjectScale: 1\n"
            "    secondaryUVPackMargin: 4\n"
            "    useFileScale: 1\n"
            "    strictVertexDataChecks: 0\n"
            "  tangentSpace:\n"
            "    normalSmoothAngle: 60\n"
            "    normalImportMode: 0\n"
            "    tangentImportMode: 3\n"
            "    normalCalculationMode: 4\n"
            "    legacyComputeAllNormalsFromSmoothingGroupsWhenMeshHasBlendShapes: 0\n"
            "    blendShapeNormalImportMode: 1\n"
            "    normalSmoothingSource: 0\n"
            "  referencedClips: []\n"
            "  importAnimation: 1\n"
            "  humanDescription:\n"
            "    serializedVersion: 3\n"
            "    human: []\n"
            "    skeleton: []\n"
            "    armTwist: 0.5\n"
            "    foreArmTwist: 0.5\n"
            "    upperLegTwist: 0.5\n"
            "    legTwist: 0.5\n"
            "    armStretch: 0.05\n"
            "    legStretch: 0.05\n"
            "    feetSpacing: 0\n"
            "    globalScale: 1\n"
            "    rootMotionBoneName: \n"
            "    hasTranslationDoF: 0\n"
            "    hasExtraRoot: 0\n"
            "    skeletonHasParents: 1\n"
            "  lastHumanDescriptionAvatarSource: {instanceID: 0}\n"
            "  autoGenerateAvatarMappingIfUnspecified: 1\n"
            "  animationType: 1\n"
            "  humanoidOversampling: 1\n"
            "  avatarSetup: 0\n"
            "  addHumanoidExtraRootOnlyWhenUsingAvatar: 1\n"
            "  importBlendShapeDeformPercent: 1\n"
            "  remapMaterialsIfMaterialImportModeIsNone: 0\n"
            "  additionalBone: 0\n"
            "  userData: \n"
            "  assetBundleName: \n"
            "  assetBundleVariant: \n" % guid)


def glb_to_fbx(glb_path: str, fbx_path: str, blender_exe: str | None = None) -> None:
    """用 Blender headless 把单个 glb 转成 FBX（局部坐标，不摆 anchor）。

    调用 `glb_to_fbx.py`（bpy 脚本），`import_scene.gltf` -> `export_scene.fbx`。
    顶点保持 glb 局部坐标，摆放交给 Unity 场景 Transform。返回 None；失败抛
    RuntimeError（带 Blender stderr 尾行）。
    """
    exe = blender_exe or BLENDER_EXE
    if not os.path.isfile(exe):
        raise FileNotFoundError(
            "blender not found at %s（用 ZENGINE_BLENDER 环境变量指定）" % exe)
    os.makedirs(os.path.dirname(fbx_path), exist_ok=True)
    cmd = [exe, "--background", "--python", GLB_TO_FBX_SCRIPT, "--",
           glb_path, fbx_path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.isfile(fbx_path):
        tail = (proc.stderr or "").strip().splitlines()[-5:]
        raise RuntimeError("glb->fbx failed for %s: %s" % (glb_path, " / ".join(tail)))


# --------------------------------------------------------------------------
# 场景文档写出
# --------------------------------------------------------------------------

class SceneWriter:
    """极简 .unity YAML 写出器（只够本模块用）。"""

    def __init__(self) -> None:
        self.documents: List[str] = []
        self._next_id = 100000

    def alloc(self) -> int:
        self._next_id += 1
        return self._next_id

    def add(self, class_id: int, object_id: int, body: str) -> None:
        self.documents.append("--- !u!%d &%d\n%s" % (class_id, object_id, body))

    def gameobject(self, go_id: int, name: str, comp_ids: Sequence[int]) -> None:
        comps = "\n".join("  - component: {fileID: %d}" % c for c in comp_ids)
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
                  children: Sequence[int], position: Sequence[float],
                  rotation_quat: Sequence[float], scale: Sequence[float]) -> None:
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
            "  m_LocalRotation: %s" % _fmt_vec4(rotation_quat),
            "  m_LocalPosition: %s" % _fmt_vec3(position),
            "  m_LocalScale: %s" % _fmt_vec3(scale),
            "  m_ConstrainProportionsScale: 0",
            child_block,
            "  m_Father: {fileID: %d}" % father_id,
            "  m_LocalEulerAnglesHint: {x: 0, y: 0, z: 0}",
            "",
        ]))

    def mesh_filter(self, mf_id: int, go_id: int, guid: str,
                    ref_type: int = 2) -> None:
        # ref_type: 2 = 独立 .asset；3 = FBX sub-asset（现网原始资产）
        self.add(33, mf_id, "\n".join([
            "MeshFilter:",
            "  m_ObjectHideFlags: 0",
            "  m_CorrespondingSourceObject: " + NULL_REF,
            "  m_PrefabInstance: " + NULL_REF,
            "  m_PrefabAsset: " + NULL_REF,
            "  m_GameObject: {fileID: %d}" % go_id,
            "  m_Mesh: {fileID: %d, guid: %s, type: %d}"
            % (MESH_MAIN_FILEID, guid, ref_type),
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

    def camera(self, camera_id: int, go_id: int, fov: float) -> None:
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
            "  field of view: %.6g" % fov,
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

    def emit(self, text: str) -> None:
        self.documents.append(text)


# --------------------------------------------------------------------------
# 摆放与旋转
# --------------------------------------------------------------------------

def quaternion_from_yaw(yaw_deg: float) -> Tuple[float, float, float, float]:
    """绕 Y 轴旋转 yaw_deg（Unity 约定：绕 Y 正转）。"""
    half = math.radians(yaw_deg) / 2
    return (0.0, math.sin(half), 0.0, math.cos(half))


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------

def load_guids(path: str) -> Dict[str, Dict[str, str]]:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def load_manifest(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def header_from_template(template_path: str | None) -> str:
    """取模板前 4 个场景级文档（OcclusionCulling/RenderSettings/Lightmap/NavMesh）。"""
    if template_path is None or not os.path.isfile(template_path):
        return ""
    with open(template_path, "r", encoding="utf-8") as handle:
        content = handle.read()
    blocks = content.split("--- !u!")
    # Unity 的 YAML 序列化器要求场景文件头部必须带这两行声明，
    # 否则会报 "File may be corrupted or was serialized with a newer version of Unity"。
    # split 会把头部丢进 blocks[0]，这里显式补回。
    header = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
    kept = 0
    for block in blocks[1:]:
        header_block = "--- !u!" + block
        class_id = block.split(" ")[0].split("&")[0].strip()
        if class_id in SCENE_LEVEL_CLASSES:
            header += header_block
            kept += 1
            if kept >= 4:
                break
    return header


# 场景 .meta 的 guid：与 .asset / 贴图不同，场景没有"内容可 hash"的稳定 ID 来源
# （场景内容每次烘焙都可能变），所以用 uuid4 派生一次并落盘 .meta。写进 .meta
# 后 guid 即固定，重跑不会漂（除非显式删 .meta）。
def write_scene_meta(path: str) -> str:
    """给 `<scene>.unity` 写 `DefaultImporter` 的 `.meta`，返回 guid。

    与 `.asset`（NativeFormatImporter，mainObjectFileID）不同，`.unity` 场景用
    `DefaultImporter`（对齐现网 `Art_Prefabs/Changjing/DaTingTest.unity.meta`）。
    guid 用 uuid4（32 位 hex），首次写后固定。
    """
    import uuid
    guid = uuid.uuid4().hex
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "fileFormatVersion: 2\n"
            "guid: %s\n"
            "timeCreated: 1731165000\n"
            "licenseType: Pro\n"
            "DefaultImporter:\n"
            "  userData: \n"
            "  assetBundleName: \n"
            "  assetBundleVariant: \n" % guid)
    return guid


def verify_scene_file(path: str, expect_roots: Sequence[int]) -> None:
    """写出场景后立即读回校验，防 `cp`/写出截断导致 Unity 空场景。

    2026-11-09 实测踩过：部署时 .unity 被截断到只剩场景级文档 + 空
    `m_Roots: []`，Unity 加载不报错但 Hierarchy 空。这里锁三处：
    ① 头部有 `%YAML 1.1` + `%TAG !u!`（否则报 corrupted）；
    ② 末尾 SceneRoots 的 m_Roots 非空、且含所有预期根 fileID；
    ③ 文件行数 >= 场景级文档之外还有 GameObject 文档（> 6 行显然不够）。
    """
    with open(path, "r", encoding="utf-8") as handle:
        content = handle.read()
    lines = content.split("\n")

    if not content.startswith("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:"):
        raise RuntimeError("scene 缺 YAML 头部声明: %s" % path)

    # 末尾 SceneRoots 段（YAML 里 SceneRoots 一定是最后一个文档）
    if "SceneRoots:" not in content:
        raise RuntimeError("scene 缺 SceneRoots 文档: %s" % path)
    tail = content[content.rindex("SceneRoots:"):]
    if "m_Roots:" not in tail or "- {fileID:" not in tail:
        raise RuntimeError("scene 的 SceneRoots.m_Roots 为空（可能被截断）: %s" % path)
    for fid in expect_roots:
        if ("{fileID: %d}" % fid) not in tail:
            raise RuntimeError("scene 根 fileID %d 未出现在 SceneRoots: %s" % (fid, path))

    if len(lines) < 50:
        raise RuntimeError("scene 行数异常（%d，疑似截断）: %s" % (len(lines), path))


def _extract_texture(asset: Dict[str, Any], manifest_root: str,
                     asset_out_dir: str) -> None:
    """抽出 glb 内嵌 PNG 贴图 + TextureImporter .meta（GUID 派生）。"""
    asset_id = asset["asset_id"]
    glb_path = os.path.join(manifest_root, asset["mesh"])
    data = external_mesh.read_bytes(glb_path)
    try:
        png = external_mesh.glb_embedded_texture(data)
        tex_path = os.path.join(asset_out_dir, asset_id + ".png")
        with open(tex_path, "wb") as handle:
            handle.write(png)
        tex_guid = hashlib.sha256(("tex:" + asset_id).encode("utf-8")).hexdigest()[:32]
        write_texture_meta(tex_path + ".meta", tex_guid)
    except (ValueError, KeyError):
        pass


def bake_asset(asset: Dict[str, Any], manifest_root: str,
               asset_out_dir: str, guids: Dict[str, Dict[str, str]]) -> str:
    """把一个 glb 烘焙成 .asset Mesh + .meta，返回 (asset_guid)。"""
    asset_id = asset["asset_id"]
    glb_rel = asset["mesh"]
    glb_path = os.path.join(manifest_root, glb_rel)
    if not os.path.isfile(glb_path):
        raise FileNotFoundError("missing glb: %s" % glb_path)

    data = external_mesh.read_bytes(glb_path)
    positions, normals, uvs, indices = external_mesh.glb_indexed(data)

    # 把 -pivot_offset 烘进顶点（局部系原点对齐到 pivot）
    pivot = asset.get("pivot_offset", [0.0, 0.0, 0.0])
    if any(abs(p) > 1.0e-9 for p in pivot):
        positions = list(positions)
        for i in range(len(positions) // 3):
            positions[i * 3] -= pivot[0]
            positions[i * 3 + 1] -= pivot[1]
            positions[i * 3 + 2] -= pivot[2]

    os.makedirs(asset_out_dir, exist_ok=True)
    asset_path = os.path.join(asset_out_dir, asset_id + ".asset")
    meta_path = asset_path + ".meta"

    guid = hashlib.sha256(("mesh:" + asset_id).encode("utf-8")).hexdigest()[:32]
    write_mesh_asset(asset_path, asset_id, positions, normals, uvs, indices)
    write_mesh_meta(meta_path, guid)

    _extract_texture(asset, manifest_root, asset_out_dir)

    return guid


def bake_fbx(asset: Dict[str, Any], manifest_root: str,
             asset_out_dir: str) -> str:
    """把一个 glb 转成 FBX 原始资产 + ModelImporter .meta，返回 FBX GUID。

    FBX 保持 glb 局部坐标（不摆 anchor，不烘 pivot），是「可复用原始资产」，
    与现网 235 个 FBX 同一管线。Unity 导入后 mesh 以 type:3 + fileID:4300000
    被引用。贴图仍由调用方单独抽 PNG（FBX 不内嵌贴图字节）。
    """
    asset_id = asset["asset_id"]
    glb_rel = asset["mesh"]
    glb_path = os.path.join(manifest_root, glb_rel)
    if not os.path.isfile(glb_path):
        raise FileNotFoundError("missing glb: %s" % glb_path)

    os.makedirs(asset_out_dir, exist_ok=True)
    fbx_path = os.path.join(asset_out_dir, asset_id + ".fbx")
    glb_to_fbx(glb_path, fbx_path)

    guid = hashlib.sha256(("fbx:" + asset_id).encode("utf-8")).hexdigest()[:32]
    write_fbx_meta(fbx_path + ".meta", guid)
    return guid


def quaternion_look(pitch_deg: float, yaw_deg: float,
                    roll_deg: float) -> Tuple[float, float, float, float]:
    """从欧拉角（pitch 绕 X / yaw 绕 Y / roll 绕 Z）构造四元数，Unity 约定。"""
    import struct  # noqa: F401
    cy = math.cos(math.radians(yaw_deg) / 2)
    sy = math.sin(math.radians(yaw_deg) / 2)
    cp = math.cos(math.radians(pitch_deg) / 2)
    sp = math.sin(math.radians(pitch_deg) / 2)
    cr = math.cos(math.radians(roll_deg) / 2)
    sr = math.sin(math.radians(roll_deg) / 2)
    # ZYX 顺序（Unity euler）
    w = cr * cp * cy + sr * sp * sy
    x = sr * cp * cy - cr * sp * sy
    y = cr * sp * cy + sr * cp * sy
    z = cr * cp * sy - sr * sp * cy
    return (x, y, z, w)


def build_restored_scene(profile: Dict[str, Any], manifest: Dict[str, Any],
                         guids: Dict[str, Dict[str, str]],
                         manifest_root: str, asset_out_dir: str,
                         out_path: str, template_path: str | None,
                         use_fbx: bool = False,
                         placements_override: List[Dict[str, Any]] | None = None) -> None:
    # 确定要烘焙哪些资产：output.reuse_asset_ids 为空 -> 烘焙全部 4 个 glb
    assets = manifest["assets"]
    # placements：默认用 manifest 内置的；L2->L3 串联时用外部传入的
    # （assign.py 的 ir_to_placements 产出，来自 assigned scene-ir 节点）。
    placements = placements_override if placements_override is not None \
        else manifest["placements"]

    reuse = profile.get("output", {}).get("reuse_asset_ids") or []
    asset_by_id = {a["asset_id"]: a for a in assets}
    if reuse:
        selected_ids = [aid for aid in reuse if aid in asset_by_id]
    elif placements_override is not None:
        # L2->L3 串联：只烘焙 placements 实际用到的资产，不烘焙无关资产
        selected_ids = list(dict.fromkeys(
            p["asset_id"] for p in placements_override
            if p.get("asset_id") in asset_by_id))
    else:
        selected_ids = [a["asset_id"] for a in assets]

    # 烘焙选中资产，得到 asset_id -> mesh guid
    # use_fbx=True 时转 FBX（原始资产，type:3 引用）；否则烘焙 .asset（type:2）
    mesh_guid: Dict[str, str] = {}
    for asset_id in selected_ids:
        asset = asset_by_id[asset_id]
        if use_fbx:
            guid = bake_fbx(asset, manifest_root, asset_out_dir)
            # FBX 模式仍抽出贴图 PNG（材质侧用）
            _extract_texture(asset, manifest_root, asset_out_dir)
        else:
            guid = bake_asset(asset, manifest_root, asset_out_dir, guids)
        mesh_guid[asset_id] = guid

    ref_type = 3 if use_fbx else 2

    # 材质 GUID：优先 guids.materials 表，回退派生
    material_guids = guids.get("materials", {})

    writer = SceneWriter()
    root_go = writer.alloc()
    root_t = writer.alloc()

    child_transforms: List[int] = []
    placed_count = 0

    # 第一遍：写出每个 placement 的 GameObject/Transform/MeshFilter/MeshRenderer，
    # 记录 child transform，根节点最后写（children 需后置）。
    for placement in placements:
        asset_id = placement["asset_id"]
        if asset_id not in mesh_guid:
            continue
        instance_id = placement["instance_id"]
        anchor = placement["anchor"]
        rot_deg = placement.get("rotation_y_deg", 0.0)
        scale = placement.get("scale", 1.0)
        material = placement.get("material", "")

        mat_guid = material_guids.get(material)
        if not mat_guid:
            mat_guid = hashlib.sha256(
                ("mat:" + material).encode("utf-8")).hexdigest()[:32]

        go_id = writer.alloc()
        t_id = writer.alloc()
        mf_id = writer.alloc()
        mr_id = writer.alloc()

        quat = quaternion_from_yaw(rot_deg)
        s = (scale, scale, scale)

        writer.gameobject(go_id, instance_id, [t_id, mf_id, mr_id])
        writer.transform(t_id, go_id, root_t, [], anchor, quat, s)
        writer.mesh_filter(mf_id, go_id, mesh_guid[asset_id], ref_type)
        writer.mesh_renderer(mr_id, go_id, mat_guid)
        child_transforms.append(t_id)
        placed_count += 1

    # 根节点（children 已知后写）
    writer.gameobject(root_go, "RestoredScene", [root_t])
    writer.transform(root_t, root_go, 0, child_transforms,
                     (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0))

    # 相机：restore-profile 的 camera 参数（position + pitch/yaw/roll 构四元数）
    camera = profile.get("camera") or {}
    cam_go = writer.alloc()
    cam_t = writer.alloc()
    cam_c = writer.alloc()
    cam_pos = tuple(camera.get("position") or [0.0, 22.46, -19.64])
    pitch = float(camera.get("pitch_deg", 50.0))
    yaw = float(camera.get("yaw_deg", 0.0))
    roll = float(camera.get("roll_deg", 0.0))
    cam_quat = quaternion_look(pitch, yaw, roll)
    writer.gameobject(cam_go, "Main Camera", [cam_t, cam_c])
    writer.transform(cam_t, cam_go, root_t, [], cam_pos, cam_quat,
                     (1.0, 1.0, 1.0))
    writer.camera(cam_c, cam_go, float(camera.get("fov", 31.0)))

    # 拼装最终场景
    header = header_from_template(template_path)
    body = "\n".join(writer.documents) + "\n"
    body += ("--- !u!1660057539 &9223372036854775807\n"
             "SceneRoots:\n"
             "  m_ObjectHideFlags: 0\n"
             "  m_Roots:\n"
             "  - {fileID: %d}\n" % root_t)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(header + body)

    # 写出后立即读回校验（防截断 / 缺 YAML 头 / 空 SceneRoots），
    # 再落 .unity.meta（DefaultImporter，uuid4 guid 首次写后固定）。
    verify_scene_file(out_path, [root_t])
    scene_guid = write_scene_meta(out_path + ".meta")

    print("baked %d assets, placed %d instances" % (len(mesh_guid), placed_count))
    print("scene guid %s" % scene_guid)


def main() -> None:
    parser = argparse.ArgumentParser(description="restore-profile -> 烘焙场景")
    parser.add_argument("--profile", required=True, help="restore-profile.json 路径")
    parser.add_argument("--manifest", default=os.path.join(
        ROOT, "build", "external_mesh.manifest.json"))
    parser.add_argument("--guids", default=os.path.join(
        ROOT, "build", "asset_guids.json"))
    parser.add_argument("--asset-dir", default=os.path.join(
        ROOT, "build", "restored_assets"))
    parser.add_argument("--out", default="", help="输出 .unity 路径")
    parser.add_argument("--template", default=os.path.join(
        ROOT, "build", "scene_template.unity"))
    parser.add_argument("--to-fbx", action="store_true",
                        help="把 glb 转成 FBX 原始资产（type:3 引用），"
                             "对齐现网 235 个 FBX 的管线；默认烘焙 .asset（type:2）")
    parser.add_argument("--placements", default="",
                        help="可选：外部 placements JSON（L2 assign.py 的 "
                             "--placements-out 产出），用于 L2->L3 串联；"
                             "缺省用 manifest 内置 placements")
    args = parser.parse_args()

    with open(args.profile, "r", encoding="utf-8") as handle:
        profile = json.load(handle)
    manifest = load_manifest(args.manifest)
    guids = load_guids(args.guids)

    placements_override = None
    if args.placements:
        with open(args.placements, "r", encoding="utf-8") as handle:
            placements_override = json.load(handle).get("placements", [])

    profile_id = profile.get("id", "restored")
    out_path = args.out or os.path.join(
        ROOT, "build", "restored_scenes", profile_id + ".unity")

    build_restored_scene(profile, manifest, guids, os.path.dirname(args.manifest),
                         args.asset_dir, out_path, args.template,
                         use_fbx=args.to_fbx,
                         placements_override=placements_override)
    print("wrote scene -> %s" % out_path)
    print("baked assets -> %s" % args.asset_dir)
    if args.to_fbx:
        print("mode: FBX (type:3, 原始资产，对齐现网管线)")
    if placements_override is not None:
        print("placements: 外部输入 %d 个（L2->L3 串联）" % len(placements_override))


if __name__ == "__main__":
    main()
