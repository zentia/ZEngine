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


if __name__ == "__main__":
    unittest.main(verbosity=2)
