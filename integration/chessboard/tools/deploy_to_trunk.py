"""把参数化棋盘交付物部署到 G:\\OSGame trunk（默认 dry-run，不写外部工程）。

三份真源分开部署，互不覆盖：
  * board-spec.json + board.zbrd —— 冻结核心几何
  * style-pack.json —— AI/人工可替换的外围装饰
  * visual-pack.json —— AI 生成的天空盒 / 后处理 / 材质 / Volume 视觉层

visual-pack 的下游产物也一起部署：12 个 .mat（只改颜色，GUID 不变）、
2 张 AI 图片与对应的 .meta（固定 GUID，供 sky_gradient 材质引用全景图）。
换风格只需重跑 `python zboard.py --variants` + `python unity_scene.py`
并替换对应 JSON，核心台面不动。

style-pack.json 的 `external_mesh_contract/v1` 还引用 4 张单物体参考图
（混元 3D 图生 3D 的输入，status=reference_ready，glb 尚未生成）。这些图
按**包内相对路径**原样镜像到 `Assets/ZEngineChessboard/assets/style_refs/`，
保证 trunk 里 style-pack.json 的 `reference_image` 路径同样可解析；它们是
美术参考素材，不被任何运行时资产引用，Unity 会自行生成 .meta。

Blender 交付物（美术在 Blender 里调整）部署到
`Assets/ZEngineChessboard/Blender/`：`AC90_board.glb`（glTF 2.0，15 个 HD
部件各是独立对象）、`build_ac90.py`（bpy 脚本，导入 GLB + AI 天空 / 灯光 /
色彩管理 / Bloom 后另存 `AC90.blend`）、`sky_panorama.jpg`。本机没有
`blender.exe` / `bpy`，无法直接写二进制 `.blend`；美术运行脚本一次即得到
可编辑的 `.blend` 工程。几何仍只来自 `board.zbrd`，不在 Blender 里重写公式。

用法：
    python tools/deploy_to_trunk.py                 # 只打印计划
    python tools/deploy_to_trunk.py --apply        # 真正拷贝
    python tools/deploy_to_trunk.py --apply --force # 生成器改过代码后覆盖 Editor 脚本
    python tools/deploy_to_trunk.py --verify-only  # 检查目标是否已部署且哈希一致

交付物是 `Assets/Scenes/pvp/ART_OS_<id>_HD.unity` **一个高配场景**，不是 prefab：
`SceneTools.cs:26 ART_NODE="Artist"` 说明场景序列化的输入是场景里名为 Artist
的根节点，不是 prefab。High/Mid/Low 由项目自动降配工具从 HD 生成后美术手调。

**HD 场景完全离线生成，Unity 里不需要也不应该跑生成菜单**：
`unity_scene.py` 从 `build/scene_template.unity`（现网 HD 场景全文）里**只取
开头 4 个场景级文档**（含完整 GI / LightingSettings）作为 HEADER，其余棋盘
对象按 ZBRD + 已落地资产的 `.meta` GUID 纯文本写出，产物 89 文档 /
0 PrefabInstance（其中 3 个文档是 AI 视觉层的 ZEngineVolume）。
本脚本把它直接拷进 `Assets/Scenes/pvp/`。
离线生成是为了可复现、不依赖 Editor API；先前那串「打开场景就
`Graphics device is null` -> `exit(0)`」的闪退已查明是**另一个 GPUParticle
定制引擎分支**所致，换 `G:/OSGame/Client_proj/UnityEngine` 的稳定 Editor
后 AC90 可正常加载，与本交付文件无关。Editor 里的
`ZEngine/Chessboard Diagnostics` 菜单仍保持只读。

部署前请先运行（需要 trunk 里已有 mesh/material 资产）：
    python unity_scene.py --assets G:/OSGame/.../Project/Assets/ZEngineChessboard

部署后**仍需手动完成**（本脚本不自动跑 Unity）：
  1. **先关闭 Unity Editor**，再执行本脚本；之后重新打开工程，让 AssetDB
     以常规启动流程导入新场景。不要在菜单回调里触发资产导入或场景切换。
  2. 用稳定 Editor（`G:/OSGame/Client_proj/UnityEngine`）打开
     `Assets/Scenes/pvp/ART_OS_AC90_HD.unity`，确认只有一个 `Artist` 根、
     `SceneMesh` / `Light_Group` / `RenderSettingGroup` 与 15 个棋盘部件；
     `Project8ArtField` 的 fieldRoot / directionalLight 非空、ChangeStencil=1；
     Console 不应再出现 `Missing Prefab Asset` 或
     `Problem detected while opening the Scene file`。
  3. AI 视觉层已随本脚本部署：RenderSettings 的雾 / 环境光、
     `ZEngineVolume` 组件（复用现网 `RenderSettingsVolume` 脚本 GUID）、
     12 个 `.mat` 颜色、`Textures/sky_panorama.jpg` 全景图以及
     `sky_gradient` 材质对它的贴图绑定。打开场景确认没有
     `Missing Script` / `Missing Texture`，材质 shader 仍是
     `Assets/CustomResources/OSG_Default.mat`（不是 Standard）。
     AI 全景是「全景风格图」而非严格相机投影，若接缝/构图不合用美术要求，
     替换 `assets/sky_panorama.jpg` 后重跑生成器即可。
  4. 美术在 Blender 里打开 `Assets/ZEngineChessboard/Blender/build_ac90.py`
     并 Run Script（或 `blender -b --python build_ac90.py`），得到
     `AC90.blend` 后按需调整；`AC90_board.glb` 也可直接
     `File > Import > glTF 2.0`。坐标由 glTF importer 自动从 Y-up 转
     Z-up，不要在脚本外再旋转一次，否则棋盘会躺倒。
  5. High/Mid/Low 交给项目自动降配流程
     (`ArtTools/4.场景工具/--->HD转高中低配` / `FastAutoOptimization`)
     从 HD 派生后手调，本线不产出。
  6. 需要运行时资产时再跑 `SGameTools/场景序列化/输出美术节点`，产出
     `SceneExport/Artist/*_Artist.asset` + `*_core.bytessv`。
  7. **配置表不动**：`ResSceneCfg` / `ResPersonaliseChessboardDataBin` 等
     不在本线范围内。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from typing import Any

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import zboard  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402
import blender_export  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402
import external_mesh  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402

SPEC_PATH = os.path.join(ROOT, "board-spec.json")
STYLE_PATH = os.path.join(ROOT, "style-pack.json")
VISUAL_PATH = os.path.join(ROOT, "visual-pack.json")
ASSETS = os.path.join(ROOT, "assets")
BUILD = os.path.join(ROOT, "build")


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def plan(spec: dict[str, Any]) -> list[tuple[str, str, str]]:
    project = spec["target"]["project_root"]
    scripts = spec["target"]["scripts_root"]
    asset_dir = os.path.join(project, "Assets", "ZEngineChessboard")
    editor_dir = os.path.join(scripts, "Editor", "ZEngine")
    runtime_dir = os.path.join(scripts, "Scripts", "ZEngine")
    texture_dir = os.path.join(asset_dir, "Textures")
    # 外部单体资产走 StreamingAssets：运行时按原始字节读 glb / 缓存 / 清单，
    # 不需要 TextAsset GUID，也不需要 Unity 导入 glb（项目没有 glTF 运行库）。
    streaming = os.path.join(project, "Assets", "StreamingAssets",
                             "ZEngineChessboard")
    with open(VISUAL_PATH, "r", encoding="utf-8") as handle:
        visual = json.load(handle)
    items = [
        (os.path.join(BUILD, "board.zbrd"),
         os.path.join(asset_dir, "board.zbrd"), "共享几何（唯一真源产物）"),
        (os.path.join(BUILD, "board.manifest.json"),
         os.path.join(asset_dir, "board.manifest.json"), "清单 / 灯光 / LOD / 视觉摘要"),
        (SPEC_PATH, os.path.join(asset_dir, "board-spec.json"), "冻结核心几何契约与槽位"),
        (STYLE_PATH,
         os.path.join(asset_dir, "style-pack.json"), "AI 可替换的外围装饰风格包 + 冻结的单体资产引用契约"),
        (VISUAL_PATH,
         os.path.join(asset_dir, "visual-pack.json"), "AI 生成的天空盒 / 后处理 / 材质 / Volume"),
        (os.path.join(ROOT, "unity", "ZChessboardGenerator.cs"),
         os.path.join(editor_dir, "ZChessboardGenerator.cs"), "Unity Editor 只读诊断"),
        (os.path.join(BUILD, "ART_OS_AC90_HD.unity"),
         os.path.join(project, "Assets", "Scenes", "pvp", "ART_OS_AC90_HD.unity"),
         "离线生成的高配场景（Unity 不需要跑生成菜单）"),
    ]
    for name in sorted(visual["materials"]):
        items.append((os.path.join(BUILD, "materials", name + ".mat"),
                      os.path.join(asset_dir, "Materials", name + ".mat"),
                      "AI 材质颜色（GUID / shader 不变）"))
    for image in ("sky_panorama.jpg", "concept.jpg"):
        items.append((os.path.join(ASSETS, image),
                      os.path.join(texture_dir, image), "AI 生成贴图"))
        items.append((os.path.join(BUILD, "textures", image + ".meta"),
                      os.path.join(texture_dir, image + ".meta"),
                      "贴图固定 GUID 元数据"))
    # 契约驱动的参考图部署：只部署 external_mesh_contract 真正引用到的图片，
    # 并保持包内相对路径不变（trunk 里 style-pack.json 也能解析 reference_image）。
    with open(STYLE_PATH, "r", encoding="utf-8") as handle:
        style_pack = json.load(handle)
    contract = style_pack.get("external_mesh_contract", {})
    if contract.get("reference_images_deployed"):
        for entry in contract["items"]:
            relative = entry["reference_image"]
            suffix = zboard.package_relative(
                relative, "reference_image of `%s`" % entry.get("asset_id", "?"))
            items.append((os.path.join(ROOT, suffix),
                          os.path.join(asset_dir, suffix),
                          "单体装饰参考图（混元 3D 图生 3D 输入，美术素材）"))
    # 外部单体装饰消费链路：清单 + glb + Unity 网格缓存 + 运行时脚本。
    items.append((os.path.join(ROOT, "unity", "ZExternalMeshBinder.cs"),
                  os.path.join(runtime_dir, "ZExternalMeshBinder.cs"),
                  "Unity 运行时消费端（外部编译进 Scripts.dll，场景只放同名空节点）"))
    items.append((os.path.join(ROOT, "unity", "ZExternalMeshEditor.cs"),
                  os.path.join(editor_dir, "ZExternalMeshEditor.cs"),
                  "ZEngine 菜单：让 Unity 自己序列化 m_Script，一次性挂载"))
    items.append((os.path.join(BUILD, external_mesh.MANIFEST_NAME),
                  os.path.join(streaming, external_mesh.MANIFEST_NAME),
                  "锚点清单（三端唯一真源）"))
    for entry in contract.get("items", []):
        if entry.get("status") != "asset_ready":
            continue
        for key, note in (("mesh", "单体 glb（混元就绪后原地替换 + 改 sha256）"),
                          ("cache", "Unity ZBRD 网格缓存（由 glb 派生，哈希锁定）"),
                          ("texture", "baseColor PNG（从 glb 抽出，三端共用同一份字节）")):
            relative = entry.get(key)
            if not relative:
                continue
            suffix = zboard.package_relative(relative, "%s of `%s`"
                                             % (key, entry.get("asset_id")))
            items.append((os.path.join(ROOT, suffix),
                          os.path.join(streaming, suffix), note))
    blender_dir = os.path.join(asset_dir, "Blender")
    for name, note in (
            (blender_export.GLB_NAME, "glTF 2.0（15 个 HD 部件，Blender 直接导入）"),
            (blender_export.SCRIPT_NAME, "bpy 脚本：导入 GLB + AI 视觉层并另存 AC90.blend"),
            (blender_export.SKY_NAME, "AI 全景图（bpy 脚本的 World 环境贴图）")):
        items.append((os.path.join(BUILD, "blender", name),
                      os.path.join(blender_dir, name), note))
    return items


DEFAULT_SCRIPTS_DLL_GUID = "603043be3babbf44188404cd589ac0c7"


def scripts_dll_guid(spec: dict[str, Any]) -> str:
    """GameScripts 编译产物 Scripts.dll 的 GUID；读不到就用已验证的常量。"""
    meta = os.path.join(spec["target"]["project_root"], "Assets", "Plugins",
                        "GameScripts", "Scripts.dll.meta")
    if os.path.isfile(meta):
        with open(meta, "r", encoding="utf-8", errors="ignore") as handle:
            match = re.search(r"^guid: ([0-9a-f]{32})$", handle.read(), re.M)
        if match:
            return match.group(1)
    return DEFAULT_SCRIPTS_DLL_GUID


def scene_is_attached(path: str, dll_guid: str) -> bool:
    """Unity 菜单挂过消费端之后，ExternalMeshRoot 上会有 Scripts.dll 的 m_Script。

    不能只看场景里有没有这个 GUID：Project8ArtField 也用同一个 Scripts.dll，
    必须把范围缩到 ExternalMeshRoot 这个 GameObject 自己的组件上。
    """
    if not os.path.isfile(path):
        return False
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        text = handle.read()
    node = re.search(
        r"--- !u!1 &(\d+)\nGameObject:[\s\S]{0,1200}?m_Name: ExternalMeshRoot\n",
        text)
    if node is None:
        return False
    go_id = node.group(1)
    # Unity 重新保存后文档顺序会变（GameObject 在后、组件在前），按文档块找。
    for block in re.finditer(
            r"--- !u!114 &\d+\nMonoBehaviour:([\s\S]*?)(?=\n--- |\Z)", text):
        body = block.group(1)
        if ("m_GameObject: {fileID: %s}" % go_id) in body:
            if ("guid: " + dll_guid) in body:
                return True
    return False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="deploy chessboard deliverables to OSGame trunk")
    parser.add_argument("--apply", action="store_true", help="actually copy files")
    parser.add_argument("--force", action="store_true", help="overwrite existing .cs scripts")
    parser.add_argument("--force-scene", action="store_true",
                        help="overwrite the .unity even if Unity already attached the binder")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args(argv)

    try:
        spec = zboard.load_spec(SPEC_PATH)
    except (OSError, zboard.SpecError, json.JSONDecodeError) as exc:
        print("ERROR: bad spec: %s" % exc)
        return 1

    if spec["target"]["branch"] != "trunk":
        print("ERROR: this deploy script targets trunk, spec says %s"
              % spec["target"]["branch"])
        return 1

    manifest_path = os.path.join(BUILD, "board.manifest.json")
    zbrd_path = os.path.join(BUILD, "board.zbrd")
    if not os.path.isfile(zbrd_path) or not os.path.isfile(manifest_path):
        print("ERROR: run `python zboard.py --variants` first")
        return 1
    with open(manifest_path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    actual = sha256(zbrd_path)
    if actual != manifest["sha256"]:
        print("ERROR: ZBRD sha256 %s != manifest %s" % (actual[:16], manifest["sha256"][:16]))
        return 1

    try:
        items = plan(spec)
    except (OSError, KeyError, TypeError, zboard.SpecError,
            json.JSONDecodeError) as exc:
        print("ERROR: bad deploy plan: %s" % exc)
        return 1
    dll_guid = scripts_dll_guid(spec)
    attached = {dst for _src, dst, _note in items
                if dst.endswith(".unity") and scene_is_attached(dst, dll_guid)}

    if args.verify_only:
        missing = [dst for _src, dst, _note in items if not os.path.isfile(dst)]
        # 所有文件都要比，不能只比 zbrd：改了生成器/spec/视觉包但几何没变时，
        # 只看 zbrd hash 会误报"已部署"，Unity 里仍跑旧代码/旧材质。
        stale = [dst for src, dst, _note in items
                 if os.path.isfile(dst) and sha256(src) != sha256(dst)
                 and dst not in attached]
        print("DEPLOYED zbrd sha256=%s" % actual[:16])
        for path in missing:
            print("MISSING %s" % path)
        for path in stale:
            print("STALE %s" % path)
        for path in sorted(attached):
            print("LOCAL_EDIT %s (Unity 里已挂载消费端，保留)" % path)
        if missing or stale:
            print("PENDING: %d missing / %d stale of %d files"
                  % (len(missing), len(stale), len(items)))
            return 1
        print("OK: all %d files match the current build" % len(items))
        return 0

    print("TARGET branch=%s  unity=%s" % (spec["target"]["branch"],
                                          spec["target"]["unity_version"]))
    print("ZBRD  sha256=%s  parts=%d" % (actual[:16], manifest["parts"]))
    for src, dst, note in items:
        exists = "EXISTS" if os.path.isfile(dst) else "NEW"
        print("  [%s] %s\n         -> %s\n         %s" % (exists, src, dst, note))

    if not args.apply:
        print("\nDRY-RUN: nothing written. Re-run with --apply to deploy.")
        return 0

    roots = {spec["target"]["project_root"], spec["target"]["scripts_root"]}
    for root in roots:
        if not os.path.isdir(root):
            print("ERROR: target root not found: %s" % root)
            return 1

    for src, dst, _note in items:
        if dst in attached and not args.force_scene:
            print("SKIP (Unity 已挂载消费端，use --force-scene): %s" % dst)
            continue
        if os.path.isfile(dst) and dst.endswith(".cs") and not args.force:
            print("SKIP (use --force): %s" % dst)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print("WROTE %s" % dst)

    print("\nDeployed. Follow the 6 manual steps in this file's docstring;")
    print("deliverable = ONE HD .unity scene; lower tiers via auto-downgrade, config tables untouched.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
