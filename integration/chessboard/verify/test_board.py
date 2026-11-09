"""参数化棋盘离线验收：冻结核心 / 可变风格 / ZBRD / LOD / 变体 / CLI。

核心契约：7x8 只是 GridUtil 的**逻辑落点**，核心区是一整块连续台面；
外围装饰物与风格化几何由 style-pack.json 决定，可以整包替换。

不依赖 three.js、Unity、bpy、GPU；纯 stdlib。
"""

from __future__ import annotations

import ast
import copy
import hashlib
import json
import math
import os
import re
import struct
import sys
import tempfile
import unittest
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import zboard  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402
import unity_scene  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402
import blender_export  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402
import external_mesh  # pyright: ignore[reportImplicitRelativeImport]  # noqa: E402

SPEC_PATH = os.path.join(ROOT, "board-spec.json")
STYLE_PATH = os.path.join(ROOT, "style-pack.json")
VISUAL_PATH = os.path.join(ROOT, "visual-pack.json")


def spec():
    return zboard.load_spec(SPEC_PATH)


def style():
    return zboard.load_style_pack(STYLE_PATH, spec())


def visual():
    return zboard.load_visual_pack(VISUAL_PATH, spec())


def parts():
    return zboard.build_board(spec(), style())


def core_only():
    return zboard.build_board(spec(), zboard.empty_style_pack())


def f32(values):
    """ZBRD 是 float32 格式；把 Python float64 量化后再比较，验证编解码无损。"""
    return [struct.unpack("<f", struct.pack("<f", v))[0] for v in values]


def geometry_digest(ps):
    """只哈希几何（不含颜色），用于验证换风格包不改动冻结核心。"""
    digest = hashlib.sha256()
    for part in ps:
        digest.update(part.name.encode("utf-8"))
        mesh = part.mesh
        digest.update(struct.pack("<%df" % len(mesh.positions), *mesh.positions))
        digest.update(struct.pack("<%df" % len(mesh.normals), *mesh.normals))
        digest.update(struct.pack("<%df" % len(mesh.uvs), *mesh.uvs))
        digest.update(struct.pack("<%dI" % len(mesh.indices), *mesh.indices))
    return digest.hexdigest()


def with_style(mutator):
    pack = style()
    mutator(pack)
    return pack


class TestSpec(unittest.TestCase):

    def test_default_spec_is_valid(self):
        s = spec()
        self.assertEqual(s["schema"], "zengine.chessboard.spec/v1")
        self.assertEqual(s["target"]["branch"], "trunk")
        self.assertEqual(s["grid"]["columns"], 7)
        self.assertEqual(s["grid"]["rows"], 8)

    def test_grid_numbers_match_osgame_template(self):
        # W.玩法模板参数表.xlsx 行 4 / 模板 4：交错矩形 7x8，2150/1800 mm。
        g = spec()["grid"]
        self.assertEqual(g["layout"], "cross_rectangle")
        self.assertAlmostEqual(g["cell_width_m"], 2.15)
        self.assertAlmostEqual(g["cell_height_m"], 1.8)
        self.assertAlmostEqual(g["stagger_odd_rows_m"], 1.075)
        self.assertAlmostEqual(g["center_offset_m"][0], -5.912)
        self.assertAlmostEqual(g["center_offset_m"][2], -6.3)

    def test_scene_contract_is_one_hd_scene(self):
        scene = spec()["scene"]
        self.assertEqual(scene["quality"], "HD")
        self.assertEqual(scene["path"], "Assets/Scenes/pvp/ART_OS_AC90_HD.unity")
        self.assertEqual(scene["artist_root"], "Artist")
        self.assertEqual(scene["field_node"], "SceneMesh")
        # 以能正常打开的现网 HD 场景为母版，消除场景级状态差异。
        self.assertEqual(scene["template"],
                         "Assets/Scenes/pvp/ART_OS_AC10_HD.unity")
        self.assertTrue(os.path.isfile(os.path.join(
            ROOT, "build", unity_scene.TEMPLATE_CACHE)))

    def test_core_contract_is_frozen_and_continuous(self):
        core = spec()["core_contract"]
        self.assertTrue(core["fixed"])
        self.assertEqual(core["visual_kind"], "continuous_slab")
        self.assertFalse(core["logical_grid_is_visual"])
        self.assertIn("core_slab", core["fixed_parts"])
        self.assertIn("decoration_*", core["variable_parts"])

    def test_style_lives_outside_core_spec(self):
        s = spec()
        self.assertNotIn("decorations", s)
        self.assertNotIn("interactables", s)
        self.assertTrue(style()["decorations"])
        self.assertTrue(style()["interactables"])

    def test_reject_bad_specs(self):
        bad_cases = [
            ("grid.columns=0", "grid.columns"),
            ("grid.cell_width_m=-1", "cell sizes"),
            ("grid.shape=triangle", "unknown grid.shape"),
            ("grid.layout=hexagon", "layout must"),
            ("grid.cell_marks=tiles", "grid.cell_marks"),
            ("sky.mode=video", "sky.mode"),
            ("unity.material_template=", "material_template"),
            ("scene.quality=Mid", "only the HD scene"),
            ("scene.path=Assets/ZEngineChessboard/board.unity", "scene.path must"),
            ("scene.template=not-a-scene", "scene.template must"),
            ("scene.artist_root=", "scene.artist_root is required"),
        ]
        for override, message in bad_cases:
            with self.subTest(override=override):
                with self.assertRaisesRegex(zboard.SpecError, message):
                    zboard.apply_overrides(spec(), [override])

        mutations = [
            (lambda s: s["core_contract"].update(fixed=False), "core_contract"),
            (lambda s: s["core_contract"].update(visual_kind="checker_cells"), "core_contract"),
            (lambda s: s["core_contract"].update(logical_grid_is_visual=True), "core_contract"),
            (lambda s: s.update(decorations=[]), "style-dependent"),
            (lambda s: s.pop("anchors"), "missing `anchors`"),
            (lambda s: s.pop("decoration_zones"), "missing `decoration_zones`"),
            (lambda s: s.pop("unity"), "missing `unity`"),
            (lambda s: s["lods"]["HD"].pop("max_decorations"), "max_decorations"),
            (lambda s: s["decoration_zones"]["items"][0].update(half=[1]), "needs center"),
            (lambda s: s["decoration_zones"]["items"].append(
                dict(s["decoration_zones"]["items"][0])), "duplicate decoration zone"),
            (lambda s: s["anchors"]["items"].append(
                dict(s["anchors"]["items"][0])), "duplicate anchor id"),
        ]
        for mutator, message in mutations:
            with self.subTest(message=message):
                broken = copy.deepcopy(spec())
                mutator(broken)
                with self.assertRaisesRegex(zboard.SpecError, message):
                    zboard.validate_spec(broken)

    def test_overrides_change_geometry(self):
        changed = zboard.apply_overrides(spec(), ["grid.shape=hex", "grid.rows=6"])
        self.assertEqual(changed["grid"]["shape"], "hex")
        self.assertEqual(changed["grid"]["rows"], 6)
        self.assertEqual(spec()["grid"]["rows"], 8)  # 原 spec 不被改
        a = next(p for p in parts() if p.name == "core_slab")
        b = next(p for p in zboard.build_board(changed, style()) if p.name == "core_slab")
        self.assertNotEqual(geometry_digest([a]), geometry_digest([b]))
        with self.assertRaises(zboard.SpecError):
            zboard.apply_overrides(spec(), ["grid.no_such_key=1"])


class TestStylePack(unittest.TestCase):

    def test_style_pack_is_valid(self):
        pack = style()
        self.assertEqual(pack["schema"], zboard.STYLE_SCHEMA)
        self.assertEqual(pack["name"], "星海棋亭 Starlit Pavilion")
        self.assertEqual(len(pack["decorations"]), 4)
        self.assertEqual(len(pack["interactables"]), 2)
        self.assertIn("AI_GENERATED", pack["status"])
        self.assertNotIn("PLACEHOLDER", pack["status"])
        zboard.validate_style_pack(pack, spec())

    def test_style_pack_changes_only_variable_parts(self):
        plain = core_only()
        themed = parts()
        self.assertEqual(len(plain), 11)
        self.assertEqual(len(themed), 17)
        core_names = [p.name for p in plain]
        self.assertEqual([p.name for p in themed[:len(plain)]], core_names)
        self.assertEqual([p.name for p in themed[len(plain):]],
                         ["decoration_" + item["id"] for item in style()["decorations"]]
                         + ["interactable_" + item["id"]
                            for item in style()["interactables"]])

    def test_core_geometry_is_identical_with_or_without_style(self):
        a = [p for p in parts() if zboard.is_core_part(p.name)]
        b = core_only()
        self.assertEqual(len(a), len(b))
        self.assertEqual(geometry_digest(a), geometry_digest(b))
        # 换一份完全不同内容的风格包，核心几何依旧逐字节相同。
        alternate = with_style(lambda p: p.update(decorations=[], interactables=[]))
        c = [p for p in zboard.build_board(spec(), alternate)
             if zboard.is_core_part(p.name)]
        self.assertEqual(geometry_digest(a), geometry_digest(c))

    def test_style_pack_rejections(self):
        s = spec()
        cases = [
            (lambda p: p["decorations"].append(dict(p["decorations"][0])), "duplicate style pack id"),
            (lambda p: p["decorations"][0].update(primitive="blob"), "unknown primitive"),
            (lambda p: p["decorations"][0].update(min_lod="Ultra"), "bad min_lod"),
            (lambda p: p["interactables"][0].update(tag=""), "needs a `tag`"),
            (lambda p: p["decorations"][0].update(zone="nowhere"), "unknown zone"),
            (lambda p: p["decorations"][0].update(position=[0, 0, 0]), "outside zone"),
            (lambda p: p["decorations"][0].update(position=[1, "x", 2]), "numeric position"),
            (lambda p: p["decorations"][0].update(color=[2, 0, 0]), "color channels"),
            (lambda p: p["decorations"][0].update(external_mesh="orb.glb"), "not implemented"),
            (lambda p: p.update(grid={}), "must not override frozen core"),
            (lambda p: p.update(schema="v0"), "unsupported style pack schema"),
        ]
        for mutator, message in cases:
            with self.subTest(message=message):
                pack = with_style(mutator)
                with self.assertRaisesRegex(zboard.SpecError, message):
                    zboard.validate_style_pack(pack, s)

        packed_zone = with_style(lambda p: p["decorations"].extend(
            [dict(p["decorations"][0], id="clone_%d" % i) for i in range(8)]))
        with self.assertRaisesRegex(zboard.SpecError, "exceeds max_items"):
            zboard.validate_style_pack(packed_zone, s)
        tight = copy.deepcopy(spec())
        tight["lods"]["HD"]["max_decorations"] = 2
        with self.assertRaisesRegex(zboard.SpecError, "exceeds HD"):
            zboard.validate_style_pack(style(), tight)

    def test_decoration_primitives_rest_above_ground(self):
        for part in parts():
            if not zboard.is_variable_part(part.name):
                continue
            with self.subTest(part=part.name):
                lo, hi = part.mesh.bounds()
                self.assertGreaterEqual(lo[1], -0.01)
                self.assertTrue(any(hi[a] - lo[a] > 0.01 for a in range(3)))


class TestExternalMeshContract(unittest.TestCase):
    """第 2 步：星海棋亭主题包 + 4 张单体参考图 + 冻结的 glb 锚点引用契约。"""

    pack: dict[str, Any] = {}
    contract: dict[str, Any] = {}

    def setUp(self):
        self.pack = style()
        self.contract = self.pack["external_mesh_contract"]

    def validate(self, pack=None, root=None):
        zboard.validate_external_mesh_contract(
            self.pack if pack is None else pack, spec(), root)

    def broken(self, mutator):
        pack = copy.deepcopy(self.pack)
        mutator(pack)
        return pack

    def test_contract_is_active_with_glb_and_anchor(self):
        self.assertEqual(self.contract["schema"], zboard.EXTERNAL_MESH_SCHEMA)
        self.assertTrue(self.contract["frozen"])
        self.assertEqual(self.contract["asset_format"], "glb")
        self.assertEqual(self.contract["reference_root"], "package_root")
        self.assertTrue(self.contract["reference_images_deployed"])
        self.assertIn("IMPLEMENTED", self.contract["consumer_status"])
        self.assertIn("CONTRACT_ACTIVE", self.contract["status"])
        self.assertEqual(len(self.contract["items"]), 4)
        self.assertEqual(sum(len(e["instances"]) for e in self.contract["items"]), 6)
        for entry in self.contract["items"]:
            with self.subTest(asset=entry["asset_id"]):
                self.assertEqual(entry["status"], "asset_ready")
                for key in ("mesh", "cache"):
                    self.assertTrue(entry[key].startswith("assets/external_meshes/"))
                    self.assertTrue(entry[key].endswith(
                        ".glb" if key == "mesh" else ".zbrd"))
                self.assertEqual(len(entry["mesh_sha256"]), 64)
                self.assertEqual(len(entry["cache_sha256"]), 64)
                if entry["asset_origin"] == "proxy_zbrd":
                    # 代理 glb 的局部坐标已经是锚点相对的，不能有额外平移。
                    self.assertEqual(entry["pivot_offset"], [0.0, 0.0, 0.0])
                    continue
                # 真实资产（混元 3D 图生 3D）：glb 字节与上游产物一致，
                # 尺寸/原点差异由 pivot_offset（底面中心或体心）+ instances[].scale 表达。
                self.assertIn(entry["asset_origin"], ("hunyuan_3d", "handmade"))
                glb = external_mesh.read_bytes(os.path.join(ROOT, entry["mesh"]))
                lo, hi = external_mesh.glb_bounds(glb)
                pivot = [round(v, 6) for v in entry["pivot_offset"]]
                self.assertIn(pivot, (
                    [round(v, 6) for v in external_mesh.suggest_pivot(lo, hi)],
                    [round(v, 6) for v in external_mesh.suggest_center(lo, hi)]))
        self.validate(root=ROOT)

    def test_glb_and_cache_are_hash_locked_and_local_space(self):
        report = external_mesh.verify(self.pack, spec(), ROOT)
        self.assertEqual(report["assets"], 4)
        self.assertEqual(report["consumed"], 6)
        self.assertGreater(report["triangles"], 0)
        for entry in self.contract["items"]:
            with self.subTest(asset=entry["asset_id"]):
                glb = external_mesh.read_bytes(
                    os.path.join(ROOT, entry["mesh"]))
                self.assertEqual(external_mesh.sha256_bytes(glb),
                                 entry["mesh_sha256"])
                external_mesh._split_glb(glb)  # noqa: SLF001
                lo, hi = external_mesh.glb_bounds(glb)
                self.assertGreater(hi[0] - lo[0], 0.0)
                self.assertGreaterEqual(hi[2] - lo[2], 0.0)
                cache = external_mesh.read_bytes(
                    os.path.join(ROOT, entry["cache"]))
                self.assertEqual(external_mesh.sha256_bytes(cache),
                                 entry["cache_sha256"])
                self.assertEqual(len(zboard.decode_zbrd(cache)), 1)

    def test_unity_caches_keep_glb_indexed_vertices(self):
        """ZBRD 缓存保留 glb 的共享顶点，不再展开成 3*面数的「三角形汤」。"""
        for entry in self.contract["items"]:
            with self.subTest(asset=entry["asset_id"]):
                glb = external_mesh.read_bytes(os.path.join(ROOT, entry["mesh"]))
                doc, _blob = external_mesh._split_glb(glb)  # noqa: SLF001
                prim = doc["meshes"][0]["primitives"][0]
                glb_vertices = doc["accessors"][
                    prim["attributes"]["POSITION"]]["count"]
                glb_indices = doc["accessors"][prim["indices"]]["count"]
                cache = external_mesh.read_bytes(os.path.join(ROOT, entry["cache"]))
                mesh = zboard.decode_zbrd(cache)[0].mesh
                self.assertEqual(mesh.vertex_count, glb_vertices)
                self.assertEqual(len(mesh.indices), glb_indices)
                self.assertLess(mesh.vertex_count, len(mesh.indices),
                                "cache regressed to triangle soup")
                self.assertTrue(all(0 <= i < mesh.vertex_count
                                    for i in mesh.indices))
                # 顶点复用不能改变几何：包围盒与三角面数仍与 glb 展开结果一致。
                lo, hi = external_mesh.glb_bounds(glb)
                cache_lo, cache_hi = mesh.bounds()
                for a in range(3):
                    self.assertAlmostEqual(cache_lo[a], lo[a], places=5)
                    self.assertAlmostEqual(cache_hi[a], hi[a], places=5)
                self.assertEqual(len(mesh.indices) // 3,
                                 external_mesh.glb_triangles(glb)[3])

    def test_base_color_textures_are_extracted_and_locked(self):
        """内嵌 baseColor 贴图抽成独立 PNG，三端共用同一份字节。"""
        with open(os.path.join(ROOT, "style-pack.json"), encoding="utf-8") as handle:
            contract = json.load(handle)["external_mesh_contract"]
        for entry in contract["items"]:
            with self.subTest(asset=entry["asset_id"]):
                relative = entry.get("texture")
                self.assertEqual(relative,
                                 "assets/external_meshes/%s.png" % entry["asset_id"])
                png = external_mesh.read_bytes(os.path.join(ROOT, relative))
                glb = external_mesh.read_bytes(os.path.join(ROOT, entry["mesh"]))
                self.assertTrue(png.startswith(external_mesh.PNG_MAGIC))
                self.assertEqual(png, external_mesh.glb_embedded_texture(glb))
                self.assertEqual(hashlib.sha256(png).hexdigest(),
                                 entry["texture_sha256"])
                # Blender 脚本按包内相对路径找图，staged 副本必须逐字节相同。
                staged = os.path.join(ROOT, "build", relative.replace("/", os.sep))
                self.assertEqual(external_mesh.read_bytes(staged), png)
        with open(os.path.join(ROOT, "build", "blender", blender_export.SCRIPT_NAME),
                  encoding="utf-8") as handle:
            script = handle.read()
        for marker in ("EXTERNAL_TEX_SKIP", "node.type == \"TEX_IMAGE\"",
                       "colorspace_settings.name = \"sRGB\""):
            self.assertIn(marker, script)

    def test_anchor_math_reproduces_zbrd_world_bounds(self):
        manifest = external_mesh.build_manifest(self.pack, spec(), ROOT)
        with open(os.path.join(ROOT, "build", "board.zbrd"), "rb") as handle:
            parts = {part.name: part for part in zboard.decode_zbrd(handle.read())}
        self.assertEqual(manifest["summary"]["asset_ready"], 4)
        self.assertEqual(manifest["summary"]["placements"], 6)
        for placement in manifest["placements"]:
            with self.subTest(instance=placement["instance_id"]):
                self.assertTrue(placement["asset_ready"])
                bounds = placement["world_bounds"]
                lo, hi = parts[placement["proxy_part"]].mesh.bounds()
                asset = next(a for a in manifest["assets"]
                             if a["asset_id"] == placement["asset_id"])
                if asset["asset_origin"] != "proxy_zbrd":
                    # 真实资产不复制代理几何，只保证它占用同一个槽：XZ 中心落在
                    # 锚点；Y 上按 pivot 类型对齐——底面 pivot 站在锚点平面、
                    # 体心 pivot 让物体中心落在锚点（悬浮物）。
                    anchor = placement["anchor"]
                    pivot = asset["pivot_offset"]
                    local_min_y = asset["bounds"]["min"][1]
                    if abs(pivot[1] - local_min_y) < 1e-3:
                        self.assertAlmostEqual(bounds["min"][1], anchor[1],
                                               places=3)
                    else:
                        self.assertAlmostEqual(
                            (bounds["min"][1] + bounds["max"][1]) / 2, anchor[1],
                            places=3)
                    self.assertAlmostEqual(
                        (bounds["min"][0] + bounds["max"][0]) / 2, anchor[0],
                        places=3)
                    self.assertAlmostEqual(
                        (bounds["min"][2] + bounds["max"][2]) / 2, anchor[2],
                        places=3)
                    continue
                for axis in range(3):
                    self.assertAlmostEqual(bounds["min"][axis], lo[axis],
                                           places=4)
                    self.assertAlmostEqual(bounds["max"][axis], hi[axis],
                                           places=4)
        landed = os.path.join(ROOT, "build", external_mesh.MANIFEST_NAME)
        with open(landed, "r", encoding="utf-8") as handle:
            self.assertEqual(json.load(handle), manifest)

    def test_reference_images_exist_with_declared_hashes(self):
        self.assertEqual(self.pack["derived_from"]["concept_sha256"],
                         visual()["concept"]["sha256"])
        for entry in self.contract["items"]:
            with self.subTest(asset=entry["asset_id"]):
                path = os.path.join(ROOT, entry["reference_image"])
                self.assertTrue(os.path.isfile(path), entry["reference_image"])
                with open(path, "rb") as handle:
                    self.assertEqual(hashlib.sha256(handle.read()).hexdigest(),
                                     entry["reference_sha256"])
                self.assertEqual(entry["reference_size_px"], [1024, 1024])
        bad = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            reference_sha256="0" * 64))
        with self.assertRaisesRegex(zboard.SpecError, "sha256 mismatch"):
            self.validate(bad, ROOT)
        bad = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            reference_image="assets/style_refs/not_there.jpg"))
        with self.assertRaisesRegex(zboard.SpecError, "missing reference image"):
            self.validate(bad, ROOT)

    def test_instances_cover_every_style_item_at_its_anchor(self):
        placed = {item["id"]: item for section in ("decorations", "interactables")
                  for item in self.pack[section]}
        covered: list[str] = []
        for entry in self.contract["items"]:
            for instance in entry["instances"]:
                with self.subTest(instance=instance["id"]):
                    source = placed[instance["id"]]
                    self.assertEqual(source["theme_asset"], entry["asset_id"])
                    self.assertEqual(instance["zone"], source["zone"])
                    self.assertEqual(instance["anchor"], source["position"])
                    self.assertEqual(instance["proxy_primitive"], source["primitive"])
                    self.assertGreater(instance["scale"], 0)
                covered.append(instance["id"])
        self.assertEqual(sorted(covered), sorted(placed))

    def test_theme_colors_match_ai_visual_pack(self):
        v = visual()
        self.assertEqual(self.pack["palette"], v["palette"])
        self.assertEqual(self.pack["frame_style"]["color"],
                         v["materials"]["frame"]["color"])
        for item in self.pack["decorations"] + self.pack["interactables"]:
            with self.subTest(item=item["id"]):
                self.assertEqual(item["color"],
                                 v["materials"]["zboard_" + item["id"]]["color"])

    def test_manifest_carries_external_mesh_summary(self):
        m = zboard.manifest(spec(), parts(), zboard.encode(parts()), self.pack)
        e = m["external_mesh"]
        self.assertEqual(e["schema"], zboard.EXTERNAL_MESH_SCHEMA)
        self.assertEqual(e["asset_format"], "glb")
        self.assertTrue(e["frozen"])
        self.assertEqual(e["assets"], 4)
        self.assertEqual(e["instances"], 6)
        self.assertEqual(e["consumed"], 6)
        self.assertEqual(e["statuses"], {"asset_ready": 4})
        self.assertIn("unity", e["consumer"])
        self.assertIn("blender", e["consumer"])
        # 代理几何与材质名不变，保证已落地的 15 个 mesh / 12 个 .mat 不漂移。
        self.assertEqual(m["parts"], 17)
        self.assertEqual(len(m["core_parts"]), 11)
        self.assertEqual(len(m["variable_parts"]), 6)

    def test_pending_and_asset_ready_states(self):
        def to_pending(p):
            p["external_mesh_contract"]["items"][0].update(
                status="pending", reference_image=None, reference_sha256=None,
                reference_size_px=None, mesh=None, mesh_sha256=None,
                cache=None, cache_sha256=None,
                texture=None, texture_sha256=None, asset_origin=None)
        self.validate(self.broken(to_pending))
        with open(os.path.join(ROOT, "build", "board.zbrd"), "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        ready = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            status="asset_ready", mesh="build/board.zbrd", mesh_sha256=digest))
        self.validate(ready, ROOT)
        wrong = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            status="asset_ready", mesh="build/board.zbrd", mesh_sha256="0" * 64))
        with self.assertRaisesRegex(zboard.SpecError, "sha256 mismatch"):
            self.validate(wrong, ROOT)
        missing = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            status="asset_ready", mesh="build/no_such.glb", mesh_sha256=digest))
        with self.assertRaisesRegex(zboard.SpecError, "missing mesh"):
            self.validate(missing, ROOT)
        no_origin = self.broken(lambda p: p["external_mesh_contract"]["items"][0].update(
            asset_origin=None))
        with self.assertRaisesRegex(zboard.SpecError, "needs asset_origin"):
            self.validate(no_origin, ROOT)

    def test_contract_rejections(self):
        c = lambda p: p["external_mesh_contract"]  # noqa: E731
        first = lambda p: c(p)["items"][0]  # noqa: E731
        inst = lambda p: first(p)["instances"][0]  # noqa: E731
        cases = [
            (lambda p: c(p).update(schema="v0"), "unsupported external_mesh_contract schema"),
            (lambda p: c(p).update(frozen=False), "must be frozen"),
            (lambda p: c(p).update(asset_format="fbx"), "must be frozen"),
            (lambda p: c(p).update(reference_root="absolute"), "reference_root"),
            (lambda p: c(p).update(reference_images_deployed="yes"), "must be a bool"),
            (lambda p: c(p).update(items=[]), "non-empty list"),
            (lambda p: c(p)["items"][1].update(asset_id=first(p)["asset_id"]),
             "duplicate / empty external asset_id"),
            (lambda p: first(p).update(status="done"), "bad status"),
            (lambda p: first(p).update(reference_sha256=None),
             "needs reference_image \\+ sha256"),
            (lambda p: first(p).update(reference_sha256="nope"), "lowercase sha256"),
            (lambda p: first(p).update(reference_image="../concept.jpg"),
             "package-relative path"),
            (lambda p: first(p).update(reference_size_px=[0, 1024]),
             "reference_size_px"),
            (lambda p: first(p).update(pivot_offset=[0.0, 0.0]),
             "pivot_offset"),
            (lambda p: first(p).update(asset_origin="voxel"), "needs asset_origin"),
            (lambda p: first(p).update(status="pending", asset_origin=None,
                                       mesh="assets/meshes/a.glb"),
             "cannot declare a mesh before asset_ready"),
            (lambda p: first(p).update(status="asset_ready", mesh=None),
             "package-relative path"),
            (lambda p: first(p).update(instances=[]), "needs instances"),
            (lambda p: inst(p).update(id="unknown"), "unknown / duplicate instance"),
            (lambda p: first(p)["instances"].append(dict(inst(p))),
             "unknown / duplicate instance"),
            (lambda p: inst(p).update(zone="edge_e"), "zone mismatch"),
            (lambda p: inst(p).update(proxy_primitive="box"),
             "proxy_primitive mismatch"),
            (lambda p: inst(p).update(anchor=[0, 0, 0]),
             "anchor must equal style position"),
            (lambda p: inst(p).update(anchor=["x", 0, 0]), "numeric anchor"),
            (lambda p: inst(p).update(scale=-1), "needs scale > 0"),
            (lambda p: p["decorations"][0].update(theme_asset="halo_ring"),
             "theme_asset mismatch"),
            (lambda p: p["decorations"][0].pop("theme_asset"), "needs theme_asset"),
            (lambda p: p["decorations"][0].update(theme_asset="no_such_asset"),
             "theme_asset mismatch"),
            (lambda p: first(p)["instances"].pop(), "style items missing"),
        ]
        for mutator, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(zboard.SpecError, message):
                    self.validate(self.broken(mutator))

    def test_anchor_outside_zone_is_rejected(self):
        def mutate(p):
            p["decorations"][0]["position"] = [0.0, 0.0, 0.0]
            p["external_mesh_contract"]["items"][0]["instances"][0]["anchor"] = \
                [0.0, 0.0, 0.0]
        with self.assertRaisesRegex(zboard.SpecError, "anchor outside zone"):
            self.validate(self.broken(mutate))


class TestGridFormula(unittest.TestCase):

    def test_cell_centers_match_cross_rectangle_util(self):
        s = spec()
        # CrossRectangleUtil.cs:36-44：x = col*cw - (row&1)*cw/2 + offX
        self.assertEqual(zboard.cell_center(s, 0, 0), (-5.912, 0.0, -6.3))
        self.assertAlmostEqual(zboard.cell_center(s, 1, 0)[0], -3.762)
        self.assertAlmostEqual(zboard.cell_center(s, 0, 1)[0], -6.987)
        self.assertAlmostEqual(zboard.cell_center(s, 0, 1)[2], -4.5)
        self.assertAlmostEqual(zboard.cell_center(s, 6, 7)[0], 5.913)

    def test_align_layout_has_no_stagger(self):
        s = zboard.apply_overrides(spec(), ["grid.layout=align_rectangle"])
        self.assertEqual(zboard.cell_center(s, 0, 1)[0],
                         zboard.cell_center(s, 0, 0)[0])

    def test_logical_grid_is_not_visible_geometry(self):
        grid_parts = [p for p in parts() if p.kind == zboard.KIND_GRID]
        self.assertEqual([p.name for p in grid_parts], ["core_slab"])
        names = [p.name for p in parts()]
        self.assertFalse(any(name.startswith("cell_") for name in names))
        slab = next(p for p in parts() if p.name == "core_slab")
        lo, hi = slab.mesh.bounds()
        bounds = zboard.logical_bounds(spec())
        margin = float(spec()["grid"]["margin_m"])
        self.assertLessEqual(lo[0], bounds[0] - margin + 1e-4)
        self.assertGreaterEqual(hi[0], bounds[1] + margin - 1e-4)
        self.assertLessEqual(lo[2], bounds[2] - margin + 1e-4)
        self.assertGreaterEqual(hi[2], bounds[3] + margin - 1e-4)

    def test_engraved_marks_are_an_overlay_not_cells(self):
        s = zboard.apply_overrides(spec(), ["grid.cell_marks=engraved"])
        themed = zboard.build_board(s, style())
        names = [p.name for p in themed]
        self.assertEqual(len(names), 18)
        self.assertIn("cell_marks", names)
        self.assertEqual([p.name for p in themed if p.kind == zboard.KIND_GRID],
                         ["core_slab", "cell_marks"])
        self.assertFalse(any(name.startswith("cell_") and name != "cell_marks"
                             for name in names))


class TestGeometry(unittest.TestCase):

    def test_build_is_deterministic(self):
        a = zboard.encode(parts())
        b = zboard.encode(zboard.build_board(spec(), style()))
        self.assertEqual(a, b)
        self.assertEqual(hashlib.sha256(a).hexdigest(),
                         hashlib.sha256(b).hexdigest())

    def test_five_kinds_and_interactable_tags(self):
        ps = parts()
        self.assertEqual({p.kind for p in ps},
                         {zboard.KIND_GRID, zboard.KIND_FRAME, zboard.KIND_SKY,
                          zboard.KIND_DECORATION, zboard.KIND_INTERACTABLE})
        for p in ps:
            if p.kind == zboard.KIND_INTERACTABLE:
                self.assertTrue(p.tag.startswith("ZChessboard."))

    def test_attribute_counts_indices_and_normals(self):
        for p in parts():
            m = p.mesh
            with self.subTest(part=p.name):
                vc = m.vertex_count
                self.assertGreater(vc, 0)
                self.assertEqual(len(m.normals), 3 * vc)
                self.assertEqual(len(m.uvs), 2 * vc)
                self.assertEqual(len(m.indices), vc)
                self.assertEqual(m.indices, list(range(vc)))
                for i in range(0, len(m.normals), 3):
                    n = m.normals[i:i + 3]
                    self.assertAlmostEqual(math.sqrt(sum(v * v for v in n)),
                                           1.0, places=5)
                self.assertTrue(all(-1.01 <= v <= 1.01 for v in m.uvs))

    def test_slab_top_normals_point_up_and_sides_point_out(self):
        slab = next(p for p in parts() if p.name == "core_slab")
        m = slab.mesh
        lo, hi = m.bounds()
        center = ((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2)
        up_count = 0
        side_out = 0
        side_total = 0
        for i in range(0, len(m.normals), 3):
            n = m.normals[i:i + 3]
            if n[1] > 0.99:
                up_count += 1
            elif abs(n[1]) < 0.01:
                side_total += 1
                vx = m.positions[i] - center[0]
                vz = m.positions[i + 2] - center[2]
                if n[0] * vx + n[2] * vz > 0:
                    side_out += 1
        self.assertGreater(up_count, 0)
        self.assertGreater(side_total, 0)
        self.assertEqual(side_out, side_total)  # 侧面外法线全部朝外

    def test_sky_sphere_normals_point_inward(self):
        sky_part = next(p for p in parts() if p.name == "sky@HD")
        m = sky_part.mesh
        for i in range(0, len(m.normals), 3):
            n = m.normals[i:i + 3]
            x, y, z = m.positions[i:i + 3]
            self.assertGreater(n[0] * -x + n[1] * -y + n[2] * -z, 0.0)

    def test_style_sphere_normals_point_outward(self):
        orb = next(p for p in parts() if p.name == "decoration_orb_s")
        cx, cy, cz = next(item["position"] for item in style()["decorations"]
                          if item["id"] == "orb_s")
        m = orb.mesh
        for i in range(0, len(m.normals), 3):
            n = m.normals[i:i + 3]
            x, y, z = m.positions[i:i + 3]
            radial = (x - cx, y - cy, z - cz)
            self.assertGreater(n[0] * radial[0] + n[1] * radial[1]
                               + n[2] * radial[2], 0.0)

    def test_sky_lods_have_different_geometry(self):
        sizes = {}
        for name in zboard.LOD_BITS:
            p = next(p for p in parts() if p.name == "sky@" + name)
            sizes[name] = p.mesh.vertex_count
        self.assertEqual(len(set(sizes.values())), 4)
        self.assertGreater(sizes["HD"], sizes["Low"])

    def test_shape_variants_really_change_geometry(self):
        digests = {}
        for shape in ("rect", "chamfer", "hex"):
            s = zboard.apply_overrides(spec(), ["grid.shape=" + shape])
            slab = next(p for p in zboard.build_board(s, style())
                        if p.name == "core_slab")
            digests[shape] = geometry_digest([slab])
        self.assertEqual(len(set(digests.values())), 3)


class TestZBRD(unittest.TestCase):

    def test_roundtrip(self):
        original = parts()
        data = zboard.encode(original)
        decoded = zboard.decode_zbrd(data)
        self.assertEqual(len(decoded), len(original))
        for a, b in zip(original, decoded):
            with self.subTest(part=a.name):
                self.assertEqual(a.name, b.name)
                self.assertEqual(a.kind, b.kind)
                self.assertEqual(a.lod_mask, b.lod_mask)
                self.assertEqual(a.material, b.material)
                self.assertEqual(a.tag, b.tag)
                self.assertEqual(f32(a.color), list(b.color))
                self.assertEqual(f32(a.mesh.positions), b.mesh.positions)
                self.assertEqual(f32(a.mesh.normals), b.mesh.normals)
                self.assertEqual(f32(a.mesh.uvs), b.mesh.uvs)
                self.assertEqual(a.mesh.indices, b.mesh.indices)

    def test_header(self):
        ps = parts()
        data = zboard.encode(ps)
        magic, version, count, flags = struct.unpack_from("<4I", data, 0)
        self.assertEqual(magic, zboard.MAGIC)
        self.assertEqual(version, 1)
        self.assertEqual(count, len(ps))
        self.assertEqual(flags, 0)

    def test_reject_damaged_data(self):
        good = zboard.encode(parts())
        with self.assertRaisesRegex(zboard.ZBoardError, "too short"):
            zboard.decode_zbrd(good[:8])
        with self.assertRaisesRegex(zboard.ZBoardError, "bad ZBRD magic"):
            zboard.decode_zbrd(b"XXXX" + good[4:])
        with self.assertRaisesRegex(zboard.ZBoardError, "unsupported ZBRD version"):
            zboard.decode_zbrd(good[:4] + struct.pack("<I", 9) + good[8:])
        with self.assertRaisesRegex(zboard.ZBoardError, "truncated|trailing"):
            zboard.decode_zbrd(good[:-7])
        with self.assertRaisesRegex(zboard.ZBoardError, "trailing bytes"):
            zboard.decode_zbrd(good + b"junk")

    def test_encode_rejects_bad_indices(self):
        broken = parts()
        broken[0].mesh.indices[0] = 999999
        with self.assertRaises(zboard.ZBoardError):
            zboard.encode(broken)


class TestLodsAndManifest(unittest.TestCase):

    def test_lod_part_counts_decrease(self):
        ps = parts()
        counts = {name: len([p for p in ps if p.lod_mask & bit])
                  for name, bit in zboard.LOD_BITS.items()}
        self.assertEqual(counts["HD"], max(counts.values()))
        self.assertEqual(counts["Low"], min(counts.values()))
        self.assertGreater(counts["HD"], counts["High"])
        self.assertGreater(counts["High"], counts["Mid"])
        self.assertGreater(counts["Mid"], counts["Low"])
        for name, bit in zboard.LOD_BITS.items():
            with self.subTest(lod=name):
                core = [p for p in ps
                        if zboard.is_core_part(p.name) and p.lod_mask & bit]
                self.assertIn("core_slab", [p.name for p in core])
                self.assertEqual(len([p for p in core if p.name.startswith("frame_")]), 4)
                self.assertEqual(len([p for p in core if p.name.startswith("anchor_")]), 2)
                self.assertEqual(len([p for p in core if p.name.startswith("sky@")]), 1)

    def test_core_slab_survives_low(self):
        slab = next(p for p in parts() if p.name == "core_slab")
        self.assertEqual(slab.lod_mask, zboard.ALL_LODS)

    def test_lod_decoration_budget(self):
        m = zboard.manifest(spec(), parts(), zboard.encode(parts()), style())
        for name in zboard.LOD_BITS:
            with self.subTest(lod=name):
                self.assertLessEqual(m["lods"][name]["decorations"],
                                     m["lods"][name]["max_decorations"])

    def test_manifest(self):
        s = spec()
        ps = parts()
        data = zboard.encode(ps)
        m = zboard.manifest(s, ps, data, style())
        self.assertEqual(m["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(m["zbrd_bytes"], len(data))
        self.assertEqual(m["grid"]["columns"], 7)
        self.assertEqual(m["parts"], 17)
        self.assertEqual(m["kinds"]["grid"], 1)
        self.assertEqual(m["kinds"]["sky"], 4)
        self.assertEqual(m["target_branch"], "trunk")
        self.assertEqual(m["delivered_lod"], "HD")
        self.assertEqual(m["scene"], s["scene"])
        self.assertEqual(m["scene_parts"], m["lods"]["HD"]["parts"])
        self.assertEqual(m["core_contract"]["visual_kind"], "continuous_slab")
        self.assertFalse(m["grid"]["logical_grid_is_visual"])
        self.assertEqual(m["grid"]["cell_marks"], "none")
        self.assertEqual(len(m["core_parts"]), 11)
        self.assertEqual(len(m["variable_parts"]), 6)
        self.assertEqual(len(m["anchors"]), 2)
        self.assertEqual(len(m["decoration_zones"]), 8)
        self.assertEqual(m["style_pack"]["decorations"], 4)
        self.assertEqual(m["style_pack"]["interactables"], 2)
        self.assertEqual(m["sky"]["mode"], "procedural")
        self.assertEqual(len(m["interactables"]), 2)
        self.assertTrue(all(item["hint"] for item in m["interactables"]))


class TestUnityGeneratorContract(unittest.TestCase):
    """C# 生成器无法在本机编译（没有 Unity），这里离线守住它的适配契约。"""

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(ROOT, "unity", "ZChessboardGenerator.cs"),
                  "r", encoding="utf-8") as handle:
            cls.source = handle.read()

    def test_editor_guarded_and_braces_balanced(self):
        self.assertIn("#if UNITY_EDITOR", self.source)
        self.assertIn("#endif", self.source)
        self.assertEqual(self.source.count("{"), self.source.count("}"))

    def test_no_hard_reference_to_osgame_types(self):
        self.assertNotIn("using Assets.Scripts.GameLogic", self.source)
        self.assertIn("Project8ArtField", self.source)
        self.assertIn("AppDomain.CurrentDomain.GetAssemblies()", self.source)
        self.assertIn("AddComponent(type)", self.source)

    def test_coordinate_conversion_is_complete(self):
        self.assertIn("new Vector3(-reader.ReadSingle()", self.source)
        self.assertIn("1f - reader.ReadSingle()", self.source)
        self.assertIn("part.Indices[t + 1]", self.source)
        self.assertIn("part.Indices[t + 2]", self.source)
        self.assertIn("threejs_yup_rhs_m -> unity_yup_lhs_m", self.source)

    def test_single_hd_scene_is_the_only_deliverable(self):
        # Assets/Scenes/pvp 配的是 .unity 场景；只产 HD，其余档位由项目工具派生。
        # 生成已改为离线：Unity 菜单只读，避免 GFX_TLS_DEVICE fatal。
        self.assertIn('[MenuItem("ZEngine/Chessboard Diagnostics (read-only)")]', self.source)
        self.assertIn("READ-ONLY diagnostics", self.source)
        self.assertIn("GenerateInEditorUnsafe()", self.source)
        self.assertNotIn('[MenuItem("ZEngine/Generate Chessboard HD Scene (trunk)")]',
                         self.source)
        self.assertIn("manifest.scene.path", self.source)
        self.assertIn('manifest.scene.quality != "HD"', self.source)
        self.assertIn("EditorSceneManager.SaveScene(scene, scenePath)", self.source)
        self.assertIn("EditorSceneManager.NewScene(NewSceneSetup.EmptyScene", self.source)
        self.assertIn("ReplaceArtistRoot(scene, artistName)", self.source)
        self.assertIn("BuildBoardHierarchy(manifest, selected, field.transform", self.source)
        self.assertIn('new GameObject("Light_Group")', self.source)
        self.assertIn('new GameObject("RenderSettingGroup")', self.source)
        self.assertIn("p.LodMask & HdBit", self.source)
        self.assertIn("manifest.scene_parts", self.source)
        self.assertIn("fieldRoot", self.source)
        self.assertIn("directionalLight", self.source)
        self.assertIn("ChangeStencil", self.source)
        self.assertNotIn('manifest.root_prefix + "_" + lod', self.source)

    def test_never_saves_prefab_assets_graphics_device_fatal_fix(self):
        # 2026-11-05 实测：SaveAsPrefabAsset -> Prefab file async load (worker thread)
        # -> "Fatal Error! Graphics device is null." -> Unity exit(0)，没有 dump。
        self.assertNotIn("PrefabUtility.SaveAsPrefabAsset", self.source)
        self.assertNotIn("PrefabUtility.InstantiatePrefab", self.source)
        self.assertNotIn("AssetDatabase.DeleteAsset", self.source)
        self.assertNotIn("AssetDatabase.Refresh()", self.source)
        self.assertIn("Graphics device is null", self.source)  # 根因注释必须留着
        self.assertIn("AssetDatabase.StartAssetEditing()", self.source)
        self.assertIn("AssetDatabase.StopAssetEditing()", self.source)
        self.assertIn("EditorUtility.SetDirty", self.source)
        self.assertIn("LogStep(", self.source)

    def test_material_template_fixes_custom_srp_black_board(self):
        self.assertIn("LoadMaterialTemplate", self.source)
        self.assertIn("new Material(template)", self.source)
        self.assertIn("manifest.unity.material_template", self.source)
        self.assertIn("_AlbedoColor", spec()["unity"]["color_properties"])
        self.assertIn("OSG_Default.mat", spec()["unity"]["material_template"])
        self.assertIn("renders BLACK", self.source)

    def test_art_field_refs_are_gameobjects(self):
        # 现网 HD 场景：ArtField 挂在 SceneMesh 上，fieldRoot 指向自身。
        self.assertIn("AttachArtField(field, field, lightGo)", self.source)
        self.assertIn('SetObject(serialized, "fieldRoot", fieldRoot)', self.source)
        self.assertIn('SetObject(serialized, "directionalLight", directionalLight)', self.source)
        self.assertIn('SetObject(serialized, "sceneBackGround", null)', self.source)
        self.assertIn("is still NULL after apply", self.source)

    def test_states_scene_serialization_and_leaves_config_tables_alone(self):
        self.assertIn("ART_NODE", self.source)
        self.assertIn("_Artist.asset", self.source)
        self.assertIn("SGameTools/场景序列化/输出美术节点", self.source)
        self.assertIn("SceneExportChecker", self.source)
        # 配置表不在本线范围内，不能写成「交付后必须登记」。
        self.assertIn("ResSceneCfg", self.source)
        self.assertIn("配置表", self.source)
        self.assertIn("不动", self.source)
        self.assertNotIn("在 ResSceneCfg 新增皮肤 ID", self.source)

    def test_warns_but_does_not_delete_stale_cell_assets(self):
        # trunk 里还留着旧 56-cell 版本的 74 个 mesh / 14 个 material。
        self.assertIn("ReportStaleAssets(selected)", self.source)
        self.assertIn("orphaned cell_* resources", self.source)
        self.assertIn("只报告、不删除", self.source)

    def test_generator_enforces_continuous_slab_core(self):
        self.assertIn("BoardCoreContractJson", self.source)
        self.assertIn("BoardStylePackJson", self.source)
        self.assertIn("VerifyCoreContract(manifest, parts)", self.source)
        self.assertIn('p.Name == "core_slab"', self.source)
        self.assertIn("logical grid cells must NOT be visible geometry", self.source)
        self.assertIn("core_parts = coreCount", self.source)
        self.assertIn("variable_parts = variableCount", self.source)

    def test_zbrd_field_order_matches_csharp_reader(self):
        """用一个独立 struct 读取器按 C# 的顺序读第一个 part，验证格式没漂移。"""
        data = zboard.encode(parts())
        magic, version, count, flags = struct.unpack_from("<4I", data, 0)
        self.assertEqual((magic, version, count, flags),
                         (zboard.MAGIC, 1, len(parts()), 0))
        off = 16

        def text():
            nonlocal off
            (length,) = struct.unpack_from("<I", data, off)
            off += 4
            value = data[off:off + length].decode("utf-8")
            off += length
            return value

        name = text()
        kind, lod_mask = struct.unpack_from("<2B", data, off)[:2]
        off += 4  # kind / lod_mask / pad / pad
        material, tag = text(), text()
        vc, ic = struct.unpack_from("<2I", data, off)
        off += 8
        off += 16 * 4  # matrix
        color = struct.unpack_from("<4f", data, off)
        off += 4 * 4
        first = parts()[0]
        self.assertEqual(name, "core_slab")
        self.assertEqual(name, first.name)
        self.assertEqual(kind, first.kind)
        self.assertEqual(lod_mask, first.lod_mask)
        self.assertEqual(material, first.material)
        self.assertEqual(tag, first.tag)
        self.assertEqual((vc, ic), (first.mesh.vertex_count,
                                    len(first.mesh.indices)))
        self.assertEqual(f32(first.color), list(color))
        self.assertEqual(off, 16 + 4 + len(name) + 4 + 4 + len(material)
                         + 4 + len(tag) + 8 + 64 + 16)


class TestOfflineUnityScene(unittest.TestCase):
    """离线 .unity 生成：不碰 Unity Editor API，避开 GFX_TLS_DEVICE fatal。"""

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(ROOT, "build", "ART_OS_AC90_HD.unity"),
                  "r", encoding="utf-8") as handle:
            cls.text = handle.read()
        cls.guids = unity_scene.AssetGuids.from_cache(
            os.path.join(ROOT, "build", "asset_guids.json"))
        with open(SPEC_PATH, "r", encoding="utf-8") as handle:
            cls.spec = json.load(handle)
        with open(os.path.join(ROOT, "build", "board.manifest.json"),
                  "r", encoding="utf-8") as handle:
            cls.info = json.load(handle)
        with open(os.path.join(ROOT, "build", "board.zbrd"), "rb") as handle:
            cls.parts = zboard.decode_zbrd(handle.read())

    def test_scene_passes_structural_verification(self):
        report = unity_scene.verify(self.text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertEqual(report["hd_parts"], self.info["scene_parts"])
        self.assertEqual(report["mesh_refs"], report["hd_parts"])
        self.assertEqual(report["material_refs"], report["hd_parts"])
        self.assertGreater(report["documents"], 60)

    def test_scene_has_standard_unity_header_and_one_root(self):
        self.assertTrue(self.text.startswith("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:"))
        for class_id in ("20", "29", "104", "157", "196", "1660057539"):
            self.assertIn("--- !u!%s " % class_id, self.text)
        self.assertIn("--- !u!1660057539 &9223372036854775807\nSceneRoots:", self.text)

    def test_hierarchy_and_art_field_contract(self):
        for name in ("Artist", "SceneMesh", "BoardMesh", "Sky", "Light_Group",
                     "DirectionalLight", "RenderSettingGroup", "Main Camera",
                     "P_ART_OS_AC90"):
            self.assertIn("  m_Name: " + name + "\n", self.text)
        self.assertIn("m_Script: {fileID: -365591601, guid: "
                      + unity_scene.ARTFIELD_SCRIPT_GUID, self.text)
        self.assertIn("  ChangeStencil: 1", self.text)
        self.assertIn("  sceneSlotOffset: {x: 0, y: 0.04, z: -0.06}", self.text)
        # 默认只继承场景级 4 文档：不得带进现网 AC10 的 31 个 PrefabInstance
        # （其中 11 个 m_SourcePrefab GUID 在 trunk 不存在）或旧 Artist 残留。
        self.assertNotIn("--- !u!1001", self.text)
        self.assertNotIn(unity_scene.TEMPLATE_OLD_ARTIST_NAME, self.text)
        self.assertNotIn("m_PrefabInstance: {fileID: 1", self.text)
        self.assertEqual(self.text.count("  m_Name: Artist\n"), 1)

    def test_every_hd_part_has_mesh_and_material_refs(self):
        for part in self.parts:
            if not (part.lod_mask & unity_scene.HD_BIT):
                continue
            with self.subTest(part=part.name):
                self.assertIn("  m_Name: " + part.name + "\n", self.text)
                self.assertIn(self.guids.meshes[part.name], self.text)
                self.assertIn(self.guids.materials[part.material], self.text)

    def test_sanitize_matches_csharp(self):
        self.assertEqual(unity_scene.sanitize("sky@HD"), "sky_HD")
        self.assertEqual(unity_scene.sanitize("core_slab"), "core_slab")
        self.assertEqual(unity_scene.sanitize("a.b-c"), "a_b_c")

    def test_quaternion_is_unit_length(self):
        q = unity_scene.quaternion_from_euler([48.0, -32.0, 0.0])
        self.assertAlmostEqual(sum(v * v for v in q), 1.0, places=6)

    def test_verify_rejects_dangling_refs_and_unknown_guids(self):
        # 母版里也有 fieldRoot，必须挑 ZEngine 新增文档里的那个。
        def our_field_root(line: str) -> bool:
            match = re.search(r"\{fileID: (-?\d+)\}", line)
            if match is None:
                return False
            value = int(match.group(1))
            return (line.strip().startswith("fieldRoot:")
                    and (100000 <= value < 200000
                         or unity_scene.TEMPLATE_ID_BASE <= value
                         < unity_scene.TEMPLATE_ID_END))

        line = next(line for line in self.text.splitlines()
                    if our_field_root(line))
        broken = self.text.replace(line, "  fieldRoot: {fileID: 999999999}", 1)
        with self.assertRaisesRegex(ValueError, "dangling local fileID"):
            unity_scene.verify(broken, self.spec, self.info, self.parts, self.guids)
        guid = next(iter(self.guids.meshes.values()))
        broken = self.text.replace(guid, "a" * 32, 1)
        with self.assertRaisesRegex(ValueError, "unknown GUID"):
            unity_scene.verify(broken, self.spec, self.info, self.parts, self.guids)

    def test_check_cli(self):
        self.assertEqual(unity_scene.main(["--check"]), 0)

    def test_deterministic_rebuild_from_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "scene.unity")
            self.assertEqual(unity_scene.main(["--out", out]), 0)
            with open(out, "r", encoding="utf-8") as handle:
                self.assertEqual(handle.read(), self.text)

    def test_template_mode_inherits_stock_scene_state(self):
        """默认只继承现网场景级 4 文档，不带旧美术 / 缺失 prefab。"""
        report = unity_scene.verify(self.text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertFalse(report["template"])
        self.assertLess(report["documents"], 100)
        self.assertIn("  m_LightmapEditorSettings:", self.text)
        self.assertIn("m_LightingDataAsset: {fileID: 112000008", self.text)
        self.assertIn("m_LightingSettings: {fileID: 4890085278179872738",
                      self.text)
        self.assertNotIn("--- !u!1001", self.text)
        self.assertNotIn(unity_scene.TEMPLATE_OLD_ARTIST_NAME, self.text)
        roots_chunk = next(c for c in self.text.split("--- !u!")[1:]
                           if c.split("\n", 1)[0].strip().startswith(
                               "1660057539 "))
        self.assertIn("  - {fileID: %d}\n" % report["root"], roots_chunk)

    def test_header_from_template_rejects_malformed_source(self):
        with self.assertRaisesRegex(ValueError, "4 scene-level documents"):
            unity_scene.header_from_template(
                "%YAML 1.1\n--- !u!1 &1\nGameObject:\n")

    def test_full_template_mode_still_available_for_diagnostics(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "full.unity")
            self.assertEqual(unity_scene.main(
                ["--full-template", "--out", out]), 0)
            with open(out, "r", encoding="utf-8") as handle:
                text = handle.read()
        report = unity_scene.verify(text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertTrue(report["template"])
        self.assertGreater(report["documents"], 1400)
        self.assertIn(unity_scene.TEMPLATE_OLD_ARTIST_NAME, text)

    def test_no_template_fallback_produces_standalone_scene(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "standalone.unity")
            self.assertEqual(unity_scene.main(["--no-template", "--out", out]), 0)
            with open(out, "r", encoding="utf-8") as handle:
                text = handle.read()
        report = unity_scene.verify(text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertFalse(report["template"])
        self.assertLess(report["documents"], 100)

    def test_ai_visual_layer_lands_in_scene(self):
        v = visual()
        report = unity_scene.verify(self.text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertTrue(report["volume"])
        self.assertTrue(report["visual"])
        self.assertIn("  m_Name: ZEngineVolume\n", self.text)
        self.assertIn("guid: " + v["volume"]["script_guid"], self.text)
        self.assertIn("BloomThreshold: %.6f"
                      % v["post_processing"]["bloom"]["threshold"], self.text)
        self.assertIn("ExposureValue: %.6f"
                      % v["post_processing"]["exposure_value"], self.text)
        self.assertIn("m_Fog: %d" % v["skybox"]["fog"]["enabled"], self.text)
        self.assertIn("m_AmbientIntensity: %.6f"
                      % v["lighting"]["ambient_intensity"], self.text)
        # 现网 Volume 模板里指向 AC10 场景对象的引用必须清零。
        self.assertNotIn("UserDefinedBounds: {fileID: 844497555}", self.text)
        self.assertNotIn("Root: {fileID: 322305032}", self.text)
        self.assertNotIn("--- !u!1001", self.text)
        self.assertEqual(self.text.count("m_Name: Artist\n"), 1)

    def test_no_visual_scene_has_no_volume(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "no_visual.unity")
            self.assertEqual(unity_scene.main(["--no-visual", "--out", out]), 0)
            with open(out, "r", encoding="utf-8") as handle:
                text = handle.read()
        report = unity_scene.verify(text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertFalse(report["volume"])
        self.assertNotIn("ZEngineVolume", text)

    def test_external_mesh_marker_never_references_a_cs_meta_guid(self):
        report = unity_scene.verify(self.text, self.spec, self.info,
                                    self.parts, self.guids)
        self.assertTrue(report["external"])
        self.assertIn("  m_Name: %s\n" % unity_scene.EXTERNAL_NODE_NAME, self.text)
        # trunk 的 GameScripts 编译进 Scripts.dll：写 .cs.meta 的 GUID 必然 Missing。
        self.assertNotIn(unity_scene.LEGACY_BINDER_SCRIPT_GUID, self.text)
        self.assertFalse(os.path.exists(os.path.join(
            ROOT, "unity", "ZExternalMeshBinder.cs.meta")))
        with open(os.path.join(ROOT, "unity", "ZExternalMeshBinder.cs"),
                  "r", encoding="utf-8") as handle:
            binder = handle.read()
        # 不在运行时找对象：由 ZEngine 菜单在编辑期用 Unity 自己序列化 m_Script。
        self.assertNotIn("RuntimeInitializeOnLoadMethod", binder)
        self.assertIn("Application.streamingAssetsPath", binder)
        with open(os.path.join(ROOT, "unity", "ZExternalMeshEditor.cs"),
                  "r", encoding="utf-8") as handle:
            editor = handle.read()
        self.assertIn("ZEngine/棋盘/挂载外部网格消费端", editor)
        self.assertIn("Undo.AddComponent", editor)
        self.assertIn("EditorSceneManager.SaveScene", editor)
        self.assertIn("TryGetGUIDAndLocalFileIdentifier", editor)


class TestVisualPack(unittest.TestCase):
    """AI 视觉层：天空盒 / 后处理 / 材质 / Volume 必须可离线校验。"""

    def test_visual_pack_is_valid_and_covers_four_dimensions(self):
        v = visual()
        self.assertEqual(v["schema"], zboard.VISUAL_SCHEMA)
        for section in zboard.VISUAL_SECTIONS:
            self.assertIn(section, v)
        expected = set(v["ai_generation_contract"]["expected_fields"])
        self.assertTrue({"skybox", "post_processing", "materials", "volume"}
                        <= expected)
        zboard.validate_visual_pack(v, spec())

    def test_ai_images_exist_with_declared_hashes(self):
        v = visual()
        for relative, digest in (
                (v["concept"]["image"], v["concept"]["sha256"]),
                (v["skybox"]["panorama_image"], v["skybox"]["panorama_sha256"])):
            with self.subTest(image=relative):
                path = os.path.join(ROOT, relative)
                self.assertTrue(os.path.isfile(path), relative)
                with open(path, "rb") as handle:
                    self.assertEqual(hashlib.sha256(handle.read()).hexdigest(),
                                     digest)
        self.assertEqual(v["skybox"]["panorama_size_px"], [2048, 1024])

    def test_visual_pack_rejections(self):
        cases = [
            (lambda v: v.update(schema="v0"), "unsupported visual pack schema"),
            (lambda v: v.update(grid={}), "must not override frozen core"),
            (lambda v: v.pop("volume"), "missing `visual.volume`"),
            (lambda v: v["concept"].update(sha256="nope"), "lowercase sha256"),
            (lambda v: v["skybox"].update(mode="video"), "skybox.mode"),
            (lambda v: v["skybox"].update(panorama_size_px=[1024, 1024]), "2:1"),
            (lambda v: v["skybox"]["fog"].update(color=[2, 0, 0, 1]),
             "color channels"),
            (lambda v: v["post_processing"]["bloom"].update(tint=[2, 0, 0, 1]),
             "color channels"),
            (lambda v: v["post_processing"]["color_grading"].update(
                global_saturation=[1, 1, 1]), "must be 4 numbers"),
            (lambda v: v["post_processing"]["lut"].update(texture="lut.png"),
             "LUT texture assets"),
            (lambda v: v["volume"].update(script_guid="xyz"), "32-char hex GUID"),
            (lambda v: v["volume"].update(weight=2.0), "out of range"),
            (lambda v: v["lighting"].update(directional_color=[2, 0, 0, 1]),
             "color channels"),
            (lambda v: v["ai_generation_contract"].update(
                expected_fields=["palette"]), "must cover"),
        ]
        for mutator, message in cases:
            with self.subTest(message=message):
                bad = copy.deepcopy(visual())
                mutator(bad)
                with self.assertRaisesRegex(zboard.SpecError, message):
                    zboard.validate_visual_pack(bad, spec())

    def test_visual_materials_match_landed_unity_assets(self):
        guids = unity_scene.AssetGuids.from_cache(
            os.path.join(ROOT, "build", unity_scene.GUID_CACHE))
        zboard.validate_visual_materials(visual(), sorted(guids.materials))
        broken = copy.deepcopy(visual())
        broken["materials"].pop("core_slab")
        with self.assertRaisesRegex(zboard.SpecError, "landed Unity materials"):
            zboard.validate_visual_materials(broken, sorted(guids.materials))

    def test_manifest_carries_visual_summary(self):
        v = visual()
        m = zboard.manifest(spec(), parts(), zboard.encode(parts()), style(), v)
        self.assertEqual(m["visual"]["schema"], zboard.VISUAL_SCHEMA)
        self.assertEqual(m["visual"]["skybox_mode"], "panorama")
        self.assertEqual(m["visual"]["panorama_sha256"],
                         v["skybox"]["panorama_sha256"])
        self.assertEqual(m["visual"]["materials"], sorted(v["materials"]))
        self.assertEqual(m["visual"]["volume"]["script_guid"],
                         v["volume"]["script_guid"])
        self.assertEqual(m["visual"]["post_processing"]["bloom"]["threshold"],
                         v["post_processing"]["bloom"]["threshold"])
        self.assertEqual(m["visual"]["fog"], v["skybox"]["fog"])
        # 冻结几何层不受视觉包影响。
        self.assertEqual(m["sky"]["mode"], "procedural")
        self.assertEqual(m["parts"], 17)

    def test_no_visual_manifest_has_no_visual_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "board.zbrd")
            self.assertEqual(zboard.main(["--no-visual", "--out", out]), 0)
            with open(os.path.join(tmp, "board.manifest.json"), "r",
                      encoding="utf-8") as handle:
                info = json.load(handle)
        self.assertNotIn("visual", info)

    def test_material_patches_and_texture_metas(self):
        v = visual()
        mat_dir = os.path.join(ROOT, "build", "materials")
        tex_dir = os.path.join(ROOT, "build", "textures")
        self.assertEqual(sorted(os.listdir(mat_dir)),
                         sorted(name + ".mat" for name in v["materials"]))
        for name, entry in v["materials"].items():
            with self.subTest(material=name):
                with open(os.path.join(mat_dir, name + ".mat"), "r",
                          encoding="utf-8") as handle:
                    text = handle.read()
                self.assertIn("- _Color: " + unity_scene.rgba(entry["color"]),
                              text)
                self.assertIn("guid: a3ef84c358036d141bdf65720453885b", text)
        with open(os.path.join(mat_dir, "sky_gradient.mat"), "r",
                  encoding="utf-8") as handle:
            sky = handle.read()
        sky_guid = unity_scene.texture_guid("sky_panorama.jpg")
        self.assertEqual(sky.count(
            "m_Texture: {fileID: 2800000, guid: %s, type: 3}" % sky_guid), 2)
        for image in unity_scene.VISUAL_IMAGES:
            with self.subTest(image=image):
                with open(os.path.join(tex_dir, image + ".meta"), "r",
                          encoding="utf-8") as handle:
                    meta = handle.read()
                self.assertIn("guid: " + unity_scene.texture_guid(image), meta)
                self.assertNotIn("00000000000000000000000000000000", meta)
        self.assertEqual(unity_scene.texture_guid("a.jpg"),
                         unity_scene.texture_guid("a.jpg"))

    def test_patch_helpers_reject_malformed_templates(self):
        v = visual()
        with self.assertRaisesRegex(ValueError, "RenderSettings"):
            unity_scene.patch_render_settings(
                "%YAML 1.1\n--- !u!1 &1\nGameObject:\n", v)
        with self.assertRaisesRegex(ValueError, "volume template document"):
            unity_scene.volume_document("--- !u!1 &1\n", "114 &1")
        with self.assertRaisesRegex(ValueError, "YAML field"):
            unity_scene.replace_yaml_field("foo: 1\n", "bar", "2")


class TestBlenderExport(unittest.TestCase):
    """美术在 Blender 里调：GLB / bpy 脚本 / AI 全景图必须可离线校验。"""

    visual: dict[str, Any] = {}
    spec: dict[str, Any] = {}
    parts: list[Any] = []
    hd: list[Any] = []
    out: str = ""
    doc: dict[str, Any] = {}
    script: str = ""

    def setUp(self):
        self.visual = visual()
        with open(SPEC_PATH, "r", encoding="utf-8") as handle:
            self.spec = json.load(handle)
        with open(os.path.join(ROOT, "build", "board.zbrd"), "rb") as handle:
            self.parts = zboard.decode_zbrd(handle.read())
        self.hd = [p for p in self.parts
                   if p.lod_mask & zboard.LOD_BITS["HD"]]
        # 自包含：不依赖其它测试/构建是否碰过 build/blender/。
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.out = temporary.name
        blender_export.export_all(self.spec, self.parts, self.visual, self.out)
        with open(os.path.join(self.out, blender_export.GLB_NAME), "rb") as handle:
            self.doc = blender_export.parse_glb(handle.read())
        with open(os.path.join(self.out, blender_export.SCRIPT_NAME), "r",
                  encoding="utf-8") as handle:
            self.script = handle.read()

    def test_glb_is_well_formed_and_matches_zbrd(self):
        self.assertEqual(self.doc["asset"]["version"], "2.0")
        self.assertEqual(self.doc["extras"]["schema"],
                         blender_export.BLENDER_SCHEMA)
        self.assertEqual(len(self.doc["meshes"]), len(self.hd))
        self.assertEqual([m["name"] for m in self.doc["meshes"]],
                         [p.name for p in self.hd])
        self.assertEqual(self.doc["scenes"][0]["name"], "ZEngine_AC90")
        root = self.doc["nodes"][self.doc["scenes"][0]["nodes"][0]]
        self.assertEqual(root["name"], "Artist")
        self.assertEqual(len(root["children"]), 2)
        self.assertEqual(len(self.doc["materials"]), 11)
        for node in self.doc["nodes"][:len(self.hd)]:
            self.assertIn("zengine_part", node["extras"])
            self.assertIn("lod_mask", node["extras"])
        for part, mesh, node in zip(self.hd, self.doc["meshes"],
                                    self.doc["nodes"]):
            with self.subTest(part=part.name):
                self.assertEqual(mesh["name"], node["name"])
                prim = mesh["primitives"][0]
                position = self.doc["accessors"][prim["attributes"]["POSITION"]]
                self.assertEqual(position["count"],
                                 len(part.mesh.positions) // 3)
                self.assertEqual(len(position["min"]), 3)
                self.assertEqual(len(position["max"]), 3)
                self.assertEqual(
                    self.doc["accessors"][prim["indices"]]["count"],
                    len(part.mesh.indices))
                self.assertEqual(node["extras"]["zengine_kind"],
                                 zboard.KIND_NAMES[part.kind])

    def test_glb_materials_use_ai_visual_colors(self):
        for material in self.doc["materials"]:
            with self.subTest(material=material["name"]):
                name = material["extras"]["zengine_material"]
                self.assertIn(name, self.visual["materials"])
                expected = blender_export.linear_color(
                    self.visual["materials"][name]["color"])
                for got, want in zip(
                        material["pbrMetallicRoughness"]["baseColorFactor"],
                        expected):
                    self.assertAlmostEqual(got, want, places=5)
        core = next(m for m in self.doc["materials"]
                    if m["name"] == "core_slab")
        self.assertLess(core["pbrMetallicRoughness"]["baseColorFactor"][0],
                        self.visual["materials"]["core_slab"]["color"][0])

    def test_bpy_script_is_valid_python_and_saves_blend(self):
        tree = ast.parse(self.script)
        assignment = next(
            node for node in tree.body if isinstance(node, ast.Assign)
            and any(isinstance(t, ast.Name) and t.id == "VISUAL"
                    for t in node.targets))
        # VISUAL 是 json.dumps 产物，含 null/true/false，不能用 literal_eval。
        source = ast.get_source_segment(self.script, assignment.value)
        assert source is not None
        embedded = json.loads(source)
        self.assertEqual(embedded["name"], self.visual["name"])
        self.assertEqual(embedded["post_processing"]["bloom"]["threshold"],
                         self.visual["post_processing"]["bloom"]["threshold"])
        self.assertEqual(embedded["lighting"]["directional_euler"],
                         self.spec["lighting"]["directional_euler"])
        for marker in ("bpy.ops.import_scene.gltf", "ShaderNodeTexEnvironment",
                       "CompositorNodeGlare", "save_as_mainfile",
                       blender_export.BLEND_NAME, "Y-up"):
            self.assertIn(marker, self.script)
        self.assertNotIn("__ZENGINE_VISUAL_JSON__", self.script)

    def test_bpy_script_embeds_external_anchors(self):
        tree = ast.parse(self.script)
        assignment = next(
            node for node in tree.body if isinstance(node, ast.Assign)
            and any(isinstance(t, ast.Name) and t.id == "EXTERNAL"
                    for t in node.targets))
        source = ast.get_source_segment(self.script, assignment.value)
        assert source is not None
        embedded = json.loads(source)
        expected = blender_export.external_subset(blender_export._load_external())
        self.assertEqual(embedded, expected)
        self.assertEqual(len(embedded["assets"]), 4)
        self.assertEqual(len(embedded["placements"]), 6)
        for marker in ("def import_external_meshes", "import_external_meshes()",
                       "hide_render = True", 'display_type = "WIRE"',
                       "math.radians", "ZEngine_External"):
            self.assertIn(marker, self.script)
        self.assertNotIn("__ZENGINE_EXTERNAL_JSON__", self.script)
        # glTF Y-up -> Blender Z-up：(x, y, z) -> (x, -z, y)，绕 Z 转正角度。
        self.assertIn("root.location = (anchor[0] - dx, -(anchor[2] - dz),",
                      self.script)
        self.assertIn("root.rotation_euler = (0.0, 0.0, radians)", self.script)
        for asset in embedded["assets"]:
            staged = os.path.join(os.path.dirname(self.out),
                                  *asset["mesh"].split("/"))
            self.assertTrue(os.path.isfile(staged), asset["asset_id"])
            with open(staged, "rb") as handle:
                self.assertEqual(hashlib.sha256(handle.read()).hexdigest(),
                                 asset["mesh_sha256"])

    def test_blender_sky_matches_visual_pack_hash(self):
        with open(os.path.join(self.out, blender_export.SKY_NAME), "rb") as handle:
            self.assertEqual(hashlib.sha256(handle.read()).hexdigest(),
                             self.visual["skybox"]["panorama_sha256"])

    def test_export_is_idempotent_across_entry_points(self):
        # zboard --variants 传内存 Part；blender_export CLI 读文件；必须同字节。
        with open(os.path.join(self.out, blender_export.GLB_NAME), "rb") as handle:
            baseline = handle.read()
        with tempfile.TemporaryDirectory() as tmp:
            report = blender_export.export_all(
                self.spec, self.parts, self.visual, tmp)
            self.assertEqual(report["hd_parts"], len(self.hd))
            with open(os.path.join(tmp, blender_export.GLB_NAME), "rb") as handle:
                self.assertEqual(handle.read(), baseline)
            with open(os.path.join(tmp, blender_export.SCRIPT_NAME), "r",
                      encoding="utf-8") as handle:
                self.assertEqual(handle.read(), self.script)
        # 交付产物不能 stale（例如被临时构建覆盖过）。
        delivered = os.path.join(ROOT, "build", "blender")
        with open(os.path.join(delivered, blender_export.GLB_NAME), "rb") as handle:
            self.assertEqual(handle.read(), baseline,
                             "run `python zboard.py --variants` to refresh the GLB")

    def test_parse_glb_rejects_corrupt_data(self):
        for bad in (b"", b"not a glb", b"\0" * 64):
            with self.subTest(bad=bad[:10]):
                with self.assertRaises(ValueError):
                    blender_export.parse_glb(bad)

    def test_verify_outputs_detects_missing_script_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            blender_export.export_all(self.spec, self.parts, self.visual, tmp)
            path = os.path.join(tmp, blender_export.SCRIPT_NAME)
            with open(path, "r", encoding="utf-8") as handle:
                broken = handle.read().replace("save_as_mainfile", "save_nothing")
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(broken)
            with self.assertRaisesRegex(ValueError, "save_as_mainfile"):
                blender_export.verify_outputs(tmp, self.visual, self.hd)


class TestDeployPlan(unittest.TestCase):

    def test_dry_run_does_not_write_external_tree(self):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import deploy_to_trunk  # pyright: ignore[reportMissingImports]
        items = deploy_to_trunk.plan(spec())
        self.assertEqual(len(items), 45)
        for src, _dst, _note in items:
            self.assertTrue(os.path.isfile(src), src)
        destinations = [dst.replace("\\", "/") for _src, dst, _note in items]
        self.assertTrue(any(dst.endswith("style-pack.json") for dst in destinations))
        self.assertTrue(any(dst.endswith("visual-pack.json") for dst in destinations))
        self.assertEqual(len([d for d in destinations if d.endswith(".mat")]), 12)
        self.assertEqual(len([d for d in destinations if d.endswith(".jpg")]), 7)
        self.assertEqual(len([d for d in destinations if d.endswith(".jpg.meta")]), 2)
        self.assertEqual(len([d for d in destinations if "/style_refs/" in d]), 4)
        self.assertEqual(len([d for d in destinations if d.endswith(".glb")]), 5)
        self.assertEqual(len([d for d in destinations if d.endswith(".zbrd")]), 5)
        self.assertEqual(len([d for d in destinations if d.endswith(".png")]), 4)
        self.assertEqual(len([d for d in destinations
                              if d.endswith("ZExternalMeshBinder.cs")]), 1)
        self.assertEqual(len([d for d in destinations
                              if d.endswith("ZExternalMeshEditor.cs")]), 1)
        self.assertEqual(len([d for d in destinations
                              if d.endswith("ZExternalMeshBinder.cs.meta")]), 0)
        self.assertTrue(any(dst.endswith(
            "StreamingAssets/ZEngineChessboard/external_mesh.manifest.json")
            for dst in destinations))
        streaming_prefix = ("/StreamingAssets/ZEngineChessboard/assets/"
                            "external_meshes/")
        self.assertEqual(len([d for d in destinations
                              if streaming_prefix in d]), 12)
        # 参考图按包内相对路径镜像，trunk 里 style-pack.json 也能解析。
        contract = style()["external_mesh_contract"]
        for entry in contract["items"]:
            self.assertTrue(any(dst.endswith(entry["reference_image"])
                                for dst in destinations), entry["asset_id"])
            for key in ("mesh", "cache", "texture"):
                self.assertTrue(any(dst.endswith(entry[key])
                                    for dst in destinations), entry[key])
        self.assertTrue(any(d.endswith("build_ac90.py") for d in destinations))
        self.assertTrue(any(dst.endswith("ZChessboardGenerator.cs") for dst in destinations))
        self.assertTrue(any(dst.endswith("board.zbrd") for dst in destinations))
        self.assertTrue(any(dst.replace("\\", "/").endswith(
            "Assets/Scenes/pvp/ART_OS_AC90_HD.unity") for dst in destinations))
        self.assertEqual(spec()["target"]["branch"], "trunk")

    def test_manifest_carries_unity_lighting_and_sky(self):
        m = zboard.manifest(spec(), parts(), zboard.encode(parts()), style())
        self.assertEqual(m["root_prefix"], "P_ART_OS_AC90")
        self.assertEqual(len(m["lighting"]["directional_color"]), 4)
        self.assertEqual(len(m["sky"]["horizon_color"]), 4)
        self.assertIn("directional_euler", m["lighting"])
        self.assertEqual(m["unity"]["material_template"],
                         "Assets/CustomResources/OSG_Default.mat")
        self.assertIn("_AlbedoColor", m["unity"]["color_properties"])


class TestCLI(unittest.TestCase):

    def test_check_and_generate(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "nested", "board.zbrd")
            self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--check"]), 0)
            self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--out", out,
                                          "--set", "grid.shape=hex"]), 0)
            self.assertTrue(os.path.isfile(out))
            with open(out, "rb") as handle:
                ps = zboard.decode_zbrd(handle.read())
            self.assertEqual(len(ps), len(parts()))
            self.assertEqual(next(p for p in ps if p.name == "core_slab").kind,
                             zboard.KIND_GRID)
            manifest_path = os.path.join(tmp, "nested", "board.manifest.json")
            with open(manifest_path, "r", encoding="utf-8") as handle:
                info = json.load(handle)
            self.assertEqual(info["grid"]["shape"], "hex")
            self.assertEqual(info["grid"]["visual_kind"], "continuous_slab")
            self.assertEqual(info["delivered_lod"], "HD")
            self.assertTrue(info["scene"]["path"].endswith("_HD.unity"))
            self.assertEqual(len(info["variable_parts"]), 6)

    def test_no_style_builds_frozen_core_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "board.zbrd")
            self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--out", out,
                                          "--no-style"]), 0)
            with open(out, "rb") as handle:
                ps = zboard.decode_zbrd(handle.read())
            self.assertEqual(len(ps), 11)
            self.assertEqual([p.name for p in ps],
                             zboard.manifest(spec(), core_only(),
                                             zboard.encode(core_only()))["core_parts"])

    def test_bad_inputs_return_one(self):
        missing = os.path.join(ROOT, "does-not-exist.json")
        self.assertEqual(zboard.main(["--spec", missing, "--check"]), 1)
        self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--style", missing,
                                      "--check"]), 1)
        with tempfile.TemporaryDirectory() as tmp:
            bad = os.path.join(tmp, "bad-style.json")
            with open(bad, "w", encoding="utf-8") as handle:
                json.dump({"schema": "v0", "decorations": [], "interactables": []}, handle)
            self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--style", bad,
                                          "--check"]), 1)
            broken = copy.deepcopy(style())
            broken["external_mesh_contract"]["frozen"] = False
            contract_path = os.path.join(tmp, "bad-contract.json")
            with open(contract_path, "w", encoding="utf-8") as handle:
                json.dump(broken, handle)
            self.assertEqual(zboard.main(["--spec", SPEC_PATH, "--style",
                                          contract_path, "--check"]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
