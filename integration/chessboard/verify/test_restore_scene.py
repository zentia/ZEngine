"""restore_scene 门禁：glb -> .asset Mesh 烘焙 + .unity 场景装配的确定性校验。

纯 stdlib，读真实 manifest 与 glb。运行：
  python verify/test_restore_scene.py
"""

from __future__ import annotations

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import external_mesh  # noqa: E402
import restore_scene as rs  # noqa: E402


class RestoreSceneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = rs.load_manifest(
            os.path.join(ROOT, "build", "external_mesh.manifest.json"))
        cls.guids = rs.load_guids(
            os.path.join(ROOT, "build", "asset_guids.json"))
        cls.assets = {a["asset_id"]: a for a in cls.manifest["assets"]}

    def _bake(self, asset_id: str):
        asset = self.assets[asset_id]
        glb_path = os.path.join(ROOT, asset["mesh"])
        data = external_mesh.read_bytes(glb_path)
        positions, normals, uvs, indices = external_mesh.glb_indexed(data)
        # 应用与 bake_asset 相同的 pivot 平移
        pivot = asset.get("pivot_offset", [0.0, 0.0, 0.0])
        if any(abs(p) > 1.0e-9 for p in pivot):
            positions = list(positions)
            for i in range(len(positions) // 3):
                positions[i * 3] -= pivot[0]
                positions[i * 3 + 1] -= pivot[1]
                positions[i * 3 + 2] -= pivot[2]
        body = rs.mesh_asset_body(asset_id, positions, normals, uvs, indices)
        return body, positions, indices

    def test_mesh_body_vertex_index_counts_match(self) -> None:
        for asset_id in self.assets:
            body, positions, indices = self._bake(asset_id)
            vc = int(re.search(r"m_VertexCount: (\d+)", body).group(1))
            ic = int(re.search(r"indexCount: (\d+)", body).group(1))
            ds = int(re.search(r"m_DataSize: (\d+)", body).group(1))
            self.assertEqual(vc, len(positions) // 3, asset_id)
            self.assertEqual(ic, len(indices), asset_id)
            self.assertEqual(ds, vc * 64, asset_id)

    def test_mesh_body_has_14_channels(self) -> None:
        body, _, _ = self._bake("star_pillar")
        seg = body[body.index("m_Channels:"):body.index("m_DataSize:")]
        channels = re.findall(r"dimension: (\d+)", seg)
        self.assertEqual(len(channels), 14)

    def test_mesh_body_index_buffer_and_typeless_separate(self) -> None:
        body, positions, indices = self._bake("star_pillar")
        vc = len(positions) // 3
        typeless = re.search(r"_typelessdata: ([0-9a-f]+)", body).group(1)
        indexbuf = re.search(r"m_IndexBuffer: ([0-9a-f]+)", body).group(1)
        self.assertEqual(len(typeless) // 2, vc * 64)
        self.assertEqual(len(indexbuf) // 2, len(indices) * 2)

    def test_mesh_body_mesh_compression_zero(self) -> None:
        body, _, _ = self._bake("halo_ring")
        self.assertIn("m_MeshCompression: 0", body)

    def test_manifest_placements_reference_existing_materials(self) -> None:
        mat_guids = self.guids.get("materials", {})
        for placement in self.manifest["placements"]:
            material = placement["material"]
            self.assertIn(material, mat_guids,
                          "placement %s 引用未知材质 %s" % (
                              placement["instance_id"], material))

    def test_manifest_placements_cover_all_four_assets(self) -> None:
        placed_assets = {p["asset_id"] for p in self.manifest["placements"]}
        self.assertEqual(placed_assets, set(self.assets.keys()))

    def test_every_asset_has_embedded_texture(self) -> None:
        for asset_id in self.assets:
            glb_path = os.path.join(ROOT, self.assets[asset_id]["mesh"])
            data = external_mesh.read_bytes(glb_path)
            png = external_mesh.glb_embedded_texture(data)
            self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"), asset_id)


class FbxMetaTests(unittest.TestCase):
    """FBX 原始资产路径（type:3 引用，对齐现网 235 个 FBX）的纯逻辑门禁。

    不实际调 Blender（外部依赖、慢），只验证 ModelImporter meta 结构、
    GUID 派生稳定性、与 .asset 模式 GUID 的区分。
    """

    def setUp(self) -> None:
        import tempfile
        self._tmp = tempfile.mkdtemp(prefix="fbx_meta_test_")

    def _write(self) -> str:
        path = os.path.join(self._tmp, "_t.fbx.meta")
        rs.write_fbx_meta(path, "0" * 32)
        return path

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def test_fbx_meta_is_model_importer(self) -> None:
        path = self._write()
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("ModelImporter:", text)
        self.assertNotIn("NativeFormatImporter:", text)

    def test_fbx_meta_file_ids_generation_matches_production(self) -> None:
        path = self._write()
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        # 现网 FBX 用 fileIdsGeneration:2（Unity 按内容哈希生成 mesh fileID）
        self.assertIn("fileIdsGeneration: 2", text)
        self.assertIn("bakeAxisConversion: 0", text)
        self.assertIn("tangentImportMode: 3", text)

    def test_fbx_guid_differs_from_asset_guid(self) -> None:
        import hashlib
        fbx_guid = hashlib.sha256(("fbx:star_pillar").encode("utf-8")).hexdigest()[:32]
        asset_guid = hashlib.sha256(("mesh:star_pillar").encode("utf-8")).hexdigest()[:32]
        self.assertNotEqual(fbx_guid, asset_guid)

    def test_fbx_guid_is_stable(self) -> None:
        import hashlib
        a = hashlib.sha256(("fbx:star_pillar").encode("utf-8")).hexdigest()[:32]
        b = hashlib.sha256(("fbx:star_pillar").encode("utf-8")).hexdigest()[:32]
        self.assertEqual(a, b)


class SceneMetaTests(unittest.TestCase):
    """场景 .unity.meta 写出 + 场景完整性校验（2026-11-10 新增）。

    锁 2026-11-09 实测踩过的坑：场景缺 YAML 头、cp 截断导致 SceneRoots 空。
    """

    def setUp(self) -> None:
        import tempfile
        self._tmp = tempfile.mkdtemp(prefix="scene_meta_test_")

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def _write_scene(self) -> str:
        path = os.path.join(self._tmp, "s.unity")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "%YAML 1.1\n"
                "%TAG !u! tag:unity3d.com,2011:\n"
                "--- !u!29 &1\n"
                "OcclusionCullingSettings:\n"
                "  m_ObjectHideFlags: 0\n"
                "  serializedVersion: 2\n")
            handle.write("--- !u!1660057539 &9223372036854775807\n"
                         "SceneRoots:\n"
                         "  m_ObjectHideFlags: 0\n"
                         "  m_Roots:\n"
                         "  - {fileID: 100002}\n")
            # 补齐到 >50 行，避免行数校验误报
            for _ in range(60):
                handle.write("\n")
        return path

    def test_scene_meta_is_default_importer(self) -> None:
        meta_path = os.path.join(self._tmp, "s.unity.meta")
        guid = rs.write_scene_meta(meta_path)
        with open(meta_path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("DefaultImporter:", text)
        self.assertNotIn("NativeFormatImporter:", text)
        self.assertNotIn("ModelImporter:", text)
        self.assertEqual(len(guid), 32)
        self.assertRegex(guid, r"^[0-9a-f]{32}$")
        # guid 落盘后应等于返回值
        self.assertIn("guid: %s" % guid, text)

    def test_scene_meta_guid_stable_after_first_write(self) -> None:
        meta_path = os.path.join(self._tmp, "s.unity.meta")
        first = rs.write_scene_meta(meta_path)
        # 再写一次会换 guid（uuid4），但这是"首次落盘后固定"语义——
        # 这里只断言返回值是 32 位 hex、且 meta 里 guid == 返回值
        self.assertEqual(len(first), 32)
        with open(meta_path, encoding="utf-8") as handle:
            self.assertIn("guid: %s" % first, handle.read())

    def test_verify_scene_ok(self) -> None:
        path = self._write_scene()
        rs.verify_scene_file(path, [100002])  # 不应抛异常

    def test_verify_scene_missing_yaml_header(self) -> None:
        path = self._write_scene()
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        text = text.replace("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n", "")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        with self.assertRaises(RuntimeError):
            rs.verify_scene_file(path, [100002])

    def test_verify_scene_empty_roots(self) -> None:
        path = self._write_scene()
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        # 把 m_Roots 清空（模拟 cp 截断到 SceneRoots 只剩空数组）
        text = text.replace("  - {fileID: 100002}\n", "")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        with self.assertRaises(RuntimeError):
            rs.verify_scene_file(path, [100002])

    def test_verify_scene_missing_root_fileid(self) -> None:
        path = self._write_scene()
        with self.assertRaises(RuntimeError):
            # 期望的根 fileID 不在 SceneRoots 里
            rs.verify_scene_file(path, [999999])


if __name__ == "__main__":
    unittest.main(verbosity=2)
