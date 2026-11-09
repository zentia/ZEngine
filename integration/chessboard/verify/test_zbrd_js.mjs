// Node 侧对拍：JS 的 ZBRD 解析结果必须与 Python 生成的 manifest 完全一致。
// 这是 three.js 宣发页与 Unity 生成器共享同一份几何的机器化门禁。
// 运行：node verify/test_zbrd_js.mjs

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { parseZBRD, filterByLod, KIND, KIND_NAMES, LOD_BITS, MAGIC } from "../web/zbrd.mjs";
import {
  EXTERNAL_MANIFEST_SCHEMA, groupPlacements, placementTransform,
  sha256Hex, transformedBounds, verifySha256,
} from "../web/external_mesh.mjs";
import { WORLD_PACK_SCHEMA, mergeWorld, resolveWorldPack } from "../web/world-pack.mjs";

const root = new URL("../", import.meta.url);
const checks = [];
function check(name, fn) {
  fn();
  checks.push(name);
}

function load(relative) {
  const file = new URL(relative, root);
  const buf = readFileSync(file);
  const arrayBuffer = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  return { buf, arrayBuffer };
}

function json(relative) {
  return JSON.parse(readFileSync(new URL(relative, root), "utf8"));
}

const { buf, arrayBuffer } = load("build/board.zbrd");
const manifest = json("build/board.manifest.json");
const spec = json("board-spec.json");
const stylePack = json("style-pack.json");
const parts = parseZBRD(arrayBuffer);

check("sha256 matches Python manifest", () => {
  const digest = createHash("sha256").update(buf).digest("hex");
  assert.equal(digest, manifest.sha256);
});

check("header / part count / byte length", () => {
  assert.equal(new DataView(arrayBuffer).getUint32(0, true), MAGIC);
  assert.equal(buf.byteLength, manifest.zbrd_bytes);
  assert.equal(parts.length, manifest.parts);
});

check("kind histogram matches Python", () => {
  const kinds = {};
  for (const part of parts) {
    const key = KIND_NAMES[part.kind];
    kinds[key] = (kinds[key] ?? 0) + 1;
  }
  assert.deepEqual(kinds, manifest.kinds);
});

check("core is ONE continuous slab, not 56 visible cells", () => {
  const grid = parts.filter((p) => p.kind === KIND.GRID);
  assert.equal(grid.length, 1);
  assert.equal(grid[0].name, "core_slab");
  assert.equal(manifest.grid.visual_kind, "continuous_slab");
  assert.equal(manifest.grid.logical_grid_is_visual, false);
  assert.equal(manifest.grid.columns * manifest.grid.rows, 56);
  assert.equal(manifest.grid.shape, "chamfer");
  assert.ok(parts.every((p) => !p.name.startsWith("cell_") || p.name === "cell_marks"));
  assert.equal(manifest.core_contract.fixed, true);
});

check("delivery is ONE HD .unity scene", () => {
  assert.equal(manifest.delivered_lod, "HD");
  assert.equal(manifest.scene.quality, "HD");
  assert.equal(spec.scene.quality, "HD");
  assert.equal(manifest.scene.path, "Assets/Scenes/pvp/ART_OS_AC90_HD.unity");
  assert.ok(manifest.scene.path.startsWith("Assets/Scenes/pvp/"));
  assert.ok(manifest.scene.path.endsWith("_HD.unity"));
  assert.equal(manifest.scene.artist_root, "Artist");
  assert.equal(manifest.scene.field_node, "SceneMesh");
  assert.equal(manifest.scene_parts, manifest.lods.HD.parts);
});

check("offline HD .unity scene exists with valid YAML structure", () => {
  const scene = readFileSync(new URL("build/ART_OS_AC90_HD.unity", root), "utf8");
  assert.ok(scene.startsWith("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:"));
  assert.ok(scene.includes("  m_Name: " + manifest.scene.artist_root + "\n"));
  assert.ok(scene.includes("  m_Name: " + manifest.scene.field_node + "\n"));
  assert.ok(scene.includes("  ChangeStencil: 1"));
  assert.ok(scene.includes("SceneRoots:"));
  assert.ok(scene.includes("--- !u!20 "), "stock HD scenes all have a Camera");
  assert.ok(scene.includes("  m_Name: Main Camera\n"));
  // 默认只继承现网场景级 4 文档：GI 状态要齐，但不能带进 AC10 的
  // PrefabInstance（11 个 m_SourcePrefab GUID 在 trunk 缺失）或旧 Artist。
  assert.ok(scene.includes("  m_LightmapEditorSettings:"),
    "stock GI state inherited");
  assert.ok(scene.includes("m_LightingDataAsset: {fileID: 112000008"),
    "stock LightingDataAsset inherited");
  assert.ok(!scene.includes("Artist_AC10_Reference"), "no stock Artist residue");
  assert.ok(!scene.includes("--- !u!1001"), "no prefab instances");
  const meshRefs = (scene.match(/^  m_Mesh: \{fileID: 4300000/gm) ?? []).length;
  const matRefs = (scene.match(/^  - \{fileID: 2100000/gm) ?? []).length;
  assert.equal(meshRefs, manifest.scene_parts);
  assert.equal(matRefs, manifest.scene_parts);
});

check("core / variable partition matches manifest", () => {
  const names = parts.map((p) => p.name);
  assert.deepEqual(names.filter((n) => manifest.core_parts.includes(n)),
    manifest.core_parts);
  assert.deepEqual(names.filter((n) => manifest.variable_parts.includes(n)),
    manifest.variable_parts);
  assert.equal(manifest.core_parts.length + manifest.variable_parts.length,
    manifest.parts);
  assert.equal(manifest.core_parts.filter((n) => n.startsWith("frame_")).length, 4);
  assert.equal(manifest.core_parts.filter((n) => n.startsWith("anchor_")).length, 2);
  for (const anchor of manifest.anchors) {
    assert.ok(names.includes("anchor_" + anchor.id), anchor.id);
  }
});

check("style pack is the only source of variable parts", () => {
  assert.equal(spec.core_contract.visual_kind, "continuous_slab");
  assert.equal(spec.decorations, undefined);
  assert.equal(spec.interactables, undefined);
  assert.ok(Array.isArray(stylePack.decorations) && stylePack.decorations.length > 0);
  assert.equal(manifest.style_pack.decorations, stylePack.decorations.length);
  assert.equal(manifest.style_pack.interactables, stylePack.interactables.length);
  assert.equal(manifest.variable_parts.length,
    stylePack.decorations.length + stylePack.interactables.length);
  const zones = new Set(manifest.decoration_zones.map((z) => z.id));
  for (const item of [...stylePack.decorations, ...stylePack.interactables]) {
    assert.ok(zones.has(item.zone), `${item.id} uses unknown zone ${item.zone}`);
  }
  for (const id of stylePack.decorations.map((d) => "decoration_" + d.id)) {
    assert.ok(manifest.variable_parts.includes(id), id);
  }
  for (const id of stylePack.interactables.map((d) => "interactable_" + d.id)) {
    assert.ok(manifest.variable_parts.includes(id), id);
  }
});

check("external mesh contract is active and glb/cache are hash-locked", () => {
  const contract = stylePack.external_mesh_contract;
  assert.equal(contract.schema, "zengine.chessboard.external-mesh/v1");
  assert.equal(contract.frozen, true);
  assert.equal(contract.asset_format, "glb");
  assert.equal(contract.reference_root, "package_root");
  assert.equal(contract.reference_images_deployed, true);
  assert.ok(contract.consumer_status.includes("IMPLEMENTED"));
  assert.ok(contract.status.includes("CONTRACT_ACTIVE"));
  assert.equal(contract.items.length, 4);
  for (const entry of contract.items) {
    assert.equal(entry.status, "asset_ready", entry.asset_id);
    if (entry.asset_origin === "proxy_zbrd") {
      assert.deepEqual(entry.pivot_offset, [0, 0, 0], entry.asset_id);
    } else {
      // 真实资产（混元 3D）：原点差异由契约承担，必须是「底面中心」。
      assert.ok(["hunyuan_3d", "handmade"].includes(entry.asset_origin),
        entry.asset_id);
      assert.equal(entry.pivot_offset.length, 3, entry.asset_id);
      assert.ok(entry.pivot_offset.some((v) => v !== 0), entry.asset_id);
    }
    const image = readFileSync(new URL(entry.reference_image, root));
    assert.equal(createHash("sha256").update(image).digest("hex"),
      entry.reference_sha256, entry.asset_id);
    assert.deepEqual(entry.reference_size_px, [1024, 1024]);
    assert.equal(image.readUInt16BE(0), 0xffd8, `${entry.asset_id} must be JPEG`);
    for (const [key, digestKey, magic] of [
      ["mesh", "mesh_sha256", 0x46546c67],
      ["cache", "cache_sha256", 0x4452425a],
    ]) {
      const bytes = readFileSync(new URL(entry[key], root));
      assert.equal(createHash("sha256").update(bytes).digest("hex"),
        entry[digestKey], `${entry.asset_id}.${key}`);
      assert.equal(bytes.readUInt32LE(0), magic, `${entry.asset_id}.${key} magic`);
    }
  }
  assert.equal(stylePack.derived_from.concept_sha256,
    json("visual-pack.json").concept.sha256);
});

check("external mesh instances map 1:1 onto style items at their anchors", () => {
  const placed = new Map([...stylePack.decorations, ...stylePack.interactables]
    .map((item) => [item.id, item]));
  const covered = [];
  for (const entry of stylePack.external_mesh_contract.items) {
    for (const instance of entry.instances) {
      const source = placed.get(instance.id);
      assert.ok(source, `unknown instance ${instance.id}`);
      assert.equal(source.theme_asset, entry.asset_id, instance.id);
      assert.equal(instance.zone, source.zone, instance.id);
      assert.deepEqual(instance.anchor, source.position, instance.id);
      assert.equal(instance.proxy_primitive, source.primitive, instance.id);
      assert.ok(instance.scale > 0, instance.id);
      covered.push(instance.id);
    }
  }
  assert.deepEqual(covered.sort(), [...placed.keys()].sort());
});

check("manifest carries the external mesh summary and the themed palette", () => {
  const summary = manifest.external_mesh;
  assert.equal(summary.schema, "zengine.chessboard.external-mesh/v1");
  assert.equal(summary.asset_format, "glb");
  assert.equal(summary.frozen, true);
  assert.equal(summary.assets, 4);
  assert.equal(summary.instances, 6);
  assert.equal(summary.consumed, 6);
  assert.deepEqual(summary.statuses, { asset_ready: 4 });
  assert.ok(summary.consumer.includes("unity"));
  assert.ok(summary.consumer.includes("blender"));
  assert.deepEqual(stylePack.palette, json("visual-pack.json").palette);
  assert.ok(stylePack.status.includes("AI_GENERATED"));
  assert.ok(!stylePack.status.includes("PLACEHOLDER"));
  // 代理几何 / 材质名不变：已落地的 15 个 mesh 与 12 个 .mat 不漂移。
  assert.equal(manifest.parts, 17);
  assert.equal(manifest.variable_parts.length, 6);
});

check("JS anchor math reproduces the Python world bounds exactly", () => {
  const external = json("build/external_mesh.manifest.json");
  assert.equal(external.schema, EXTERNAL_MANIFEST_SCHEMA);
  assert.equal(external.assets.length, 4);
  assert.equal(external.placements.length, 6);
  const { assets, groups } = groupPlacements(external);
  assert.equal(groups.size, 4);
  for (const [assetId, placements] of groups) {
    const asset = assets.get(assetId);
    const placement = placements[0];
    const transform = placementTransform(placement, asset.pivot_offset);
    if (asset.asset_origin === "proxy_zbrd") {
      // 代理 glb 的局部坐标已经锚点相对：位置就是锚点，scale 恒为 1。
      assert.deepEqual(transform.position, placement.anchor, assetId);
      assert.equal(transform.scale, 1, assetId);
      continue;
    }
    // 真实资产（混元 3D）：pivot_offset 决定锚点语义——底面 pivot 站在锚点
    // 平面、体心 pivot 让物体中心落在锚点（悬浮物）；XZ 中心始终对齐锚点。
    const bounds = transformedBounds(asset.bounds, transform);
    const basePivot = Math.abs(asset.pivot_offset[1] - asset.bounds.min[1]) < 1e-3;
    const actualY = basePivot
      ? bounds.min[1]
      : (bounds.min[1] + bounds.max[1]) / 2;
    assert.ok(Math.abs(actualY - placement.anchor[1]) < 1e-3,
      `${assetId} Y ${actualY} != ${placement.anchor[1]}`);
    for (const axis of [0, 2]) {
      const center = (bounds.min[axis] + bounds.max[axis]) / 2;
      assert.ok(Math.abs(center - placement.anchor[axis]) < 1e-3,
        `${assetId} center${axis} ${center} != ${placement.anchor[axis]}`);
    }
    assert.ok(transform.scale > 0, `${assetId} scale`);
  }
  for (const placement of external.placements) {
    const asset = assets.get(placement.asset_id);
    const bounds = transformedBounds(asset.bounds,
      placementTransform(placement, asset.pivot_offset));
    for (const key of ["min", "max"]) {
      for (let axis = 0; axis < 3; axis += 1) {
        const delta = Math.abs(bounds[key][axis] - placement.world_bounds[key][axis]);
        assert.ok(delta < 1e-4,
          `${placement.instance_id}.${key}[${axis}] drift ${delta}`);
      }
    }
  }
});

check("interactables carry tags and hints", () => {
  const found = parts.filter((p) => p.kind === KIND.INTERACTABLE)
    .map((p) => ({ id: p.name, tag: p.tag }));
  assert.deepEqual(found, manifest.interactables.map((i) => ({ id: i.id, tag: i.tag })));
  assert.ok(found.every((item) => item.tag.startsWith("ZChessboard.")));
  assert.ok(manifest.interactables.every((item) => item.hint.length > 0));
});

check("attribute arrays are consistent and normals are unit length", () => {
  for (const part of parts) {
    assert.equal(part.positions.length, 3 * part.vertexCount, part.name);
    assert.equal(part.normals.length, 3 * part.vertexCount, part.name);
    assert.equal(part.uvs.length, 2 * part.vertexCount, part.name);
    assert.equal(part.indices.length, part.indexCount, part.name);
    for (let i = 0; i < part.normals.length; i += 3) {
      const length = Math.hypot(part.normals[i], part.normals[i + 1], part.normals[i + 2]);
      assert.ok(Math.abs(length - 1) < 1e-4, `${part.name} normal ${i / 3}`);
    }
    for (const uv of part.uvs) assert.ok(uv >= -0.01 && uv <= 1.01, `${part.name} uv`);
  }
});

check("LOD filtering matches Python manifest and decreases", () => {
  const counts = {};
  for (const [name, bit] of Object.entries(LOD_BITS)) {
    const selected = filterByLod(parts, name);
    assert.ok(selected.every((p) => (p.lodMask & bit) !== 0));
    counts[name] = selected.length;
    assert.equal(selected.length, manifest.lods[name].parts);
    assert.equal(selected.reduce((sum, p) => sum + p.vertexCount, 0),
      manifest.lods[name].vertices);
    assert.ok(selected.some((p) => p.name === "core_slab"), `${name} lost core_slab`);
    const decorations = selected.filter((p) => p.kind === KIND.DECORATION
      || p.kind === KIND.INTERACTABLE).length;
    assert.equal(decorations, manifest.lods[name].decorations);
    assert.ok(decorations <= manifest.lods[name].max_decorations);
  }
  assert.ok(counts.HD > counts.High && counts.High > counts.Mid
    && counts.Mid > counts.Low, JSON.stringify(counts));
});

check("sky has four distinct LOD geometries", () => {
  const skies = parts.filter((p) => p.kind === KIND.SKY);
  assert.equal(skies.length, 4);
  assert.equal(new Set(skies.map((p) => p.vertexCount)).size, 4);
});

check("variant files parse and change the slab, not the style pack", () => {
  const index = json("build/variants.json");
  const digests = [];
  for (const item of index.variants) {
    const variant = load("build/" + item.file);
    assert.equal(createHash("sha256").update(variant.buf).digest("hex"), item.sha256);
    const parsed = parseZBRD(variant.arrayBuffer);
    assert.equal(parsed.length, manifest.parts);
    assert.deepEqual(parsed.filter((p) => manifest.variable_parts.includes(p.name))
      .map((p) => p.name), manifest.variable_parts);
    const slab = parsed.find((p) => p.name === "core_slab");
    assert.ok(slab, "variant lost continuous slab");
    digests.push(Array.from(slab.positions.slice(0, 12)).join(","));
  }
  assert.equal(new Set(digests).size, index.variants.length);
});

check("damaged data is rejected", () => {
  assert.throws(() => parseZBRD(new ArrayBuffer(8)), /truncated|magic/);
  const broken = new Uint8Array(buf).slice();
  broken[0] ^= 0xff;
  assert.throws(() => parseZBRD(broken.buffer), /magic/);
  assert.throws(() => parseZBRD(arrayBuffer.slice(0, arrayBuffer.byteLength - 7)),
    /truncated|trailing/);
  const withJunk = Buffer.concat([buf, Buffer.from([1, 2])]);
  assert.throws(() => parseZBRD(withJunk.buffer.slice(
    withJunk.byteOffset, withJunk.byteOffset + withJunk.byteLength)), /trailing/);
  assert.throws(() => filterByLod(parts, "Ultra"), /unknown LOD/);
});

const visualPack = json("visual-pack.json");

check("AI visual pack covers skybox / post-processing / materials / volume", () => {
  assert.equal(visualPack.schema, "zengine.chessboard.visual-pack/v1");
  const expected = new Set(visualPack.ai_generation_contract.expected_fields);
  for (const key of ["skybox", "post_processing", "materials", "volume"]) {
    assert.ok(expected.has(key), key);
  }
  assert.equal(visualPack.skybox.mode, "panorama");
  assert.deepEqual(visualPack.skybox.panorama_size_px, [2048, 1024]);
  assert.equal(Object.keys(visualPack.materials).length, 12);
  assert.ok(visualPack.materials.anchor_info_plinth,
    "legacy info_plinth material stays only to keep landed Unity GUIDs stable");
  assert.match(visualPack.volume.script_guid, /^[0-9a-f]{32}$/);
});

check("manifest visual summary matches the AI visual pack", () => {
  assert.ok(manifest.visual, "manifest needs a visual section");
  assert.equal(manifest.visual.schema, visualPack.schema);
  assert.equal(manifest.visual.panorama_sha256, visualPack.skybox.panorama_sha256);
  assert.equal(manifest.visual.skybox_mode, visualPack.skybox.mode);
  assert.deepEqual(manifest.visual.materials,
    Object.keys(visualPack.materials).sort());
  assert.equal(manifest.visual.post_processing.bloom.threshold,
    visualPack.post_processing.bloom.threshold);
  assert.equal(manifest.visual.volume.enabled, visualPack.volume.enabled);
  assert.equal(manifest.sky.mode, "procedural", "frozen geometry sky unchanged");
});

check("AI volume and sky parameters landed in the offline scene", () => {
  const scene = readFileSync(new URL("build/ART_OS_AC90_HD.unity", root), "utf8");
  assert.ok(scene.includes("  m_Name: ZEngineVolume\n"));
  assert.ok(scene.includes(`guid: ${visualPack.volume.script_guid}`));
  assert.ok(scene.includes(`BloomThreshold: ${visualPack.post_processing.bloom.threshold.toFixed(6)}`));
  assert.ok(scene.includes(`m_Fog: ${visualPack.skybox.fog.enabled}`));
  assert.ok(!scene.includes("--- !u!1001"), "no prefab instances");
  assert.ok(!scene.includes("Root: {fileID: 322305032}"),
    "no stock-scene dangling refs");
});

check("Unity scene marks the external root without a .cs.meta GUID", () => {
  const scene = readFileSync(new URL("build/ART_OS_AC90_HD.unity", root), "utf8");
  assert.ok(scene.includes("  m_Name: ExternalMeshRoot\n"));
  // trunk 的 GameScripts 编译进 Scripts.dll，场景里写 .cs.meta GUID 必然 Missing。
  assert.ok(!scene.includes("2391662872ad428a12356ebf07318402"));
  assert.equal(existsSync(new URL("unity/ZExternalMeshBinder.cs.meta", root)),
    false, "stale .cs.meta must not be shipped");
  const binder = readFileSync(
    new URL("unity/ZExternalMeshBinder.cs", root), "utf8");
  assert.ok(!binder.includes("RuntimeInitializeOnLoadMethod"),
    "attach must happen at edit time, not at runtime");
  assert.ok(binder.includes("Application.streamingAssetsPath"));
  const editor = readFileSync(
    new URL("unity/ZExternalMeshEditor.cs", root), "utf8");
  assert.ok(editor.includes("ZEngine/棋盘/挂载外部网格消费端"));
  assert.ok(editor.includes("Undo.AddComponent"));
  assert.ok(editor.includes("EditorSceneManager.SaveScene"));
  assert.ok(editor.includes("TryGetGUIDAndLocalFileIdentifier"));
});

check("AI materials and texture metadata are deployable", () => {
  for (const name of Object.keys(visualPack.materials)) {
    const mat = readFileSync(new URL("build/materials/" + name + ".mat", root), "utf8");
    assert.ok(mat.includes("guid: a3ef84c358036d141bdf65720453885b"), name);
  }
  const sky = readFileSync(new URL("build/materials/sky_gradient.mat", root), "utf8");
  const skyMeta = readFileSync(new URL("build/textures/sky_panorama.jpg.meta", root), "utf8");
  const skyGuid = /^guid: ([0-9a-f]{32})$/m.exec(skyMeta)?.[1];
  assert.ok(skyGuid, "sky panorama meta has no GUID");
  assert.ok(sky.includes(`m_Texture: {fileID: 2800000, guid: ${skyGuid}, type: 3}`),
    "AI panorama must be bound with exactly the GUID declared in its .meta");
  for (const image of ["sky_panorama.jpg", "concept.jpg"]) {
    const meta = readFileSync(new URL("build/textures/" + image + ".meta", root), "utf8");
    assert.ok(meta.includes("fileFormatVersion: 2"), image);
    assert.ok(!meta.includes("guid: 00000000000000000000000000000000"), image);
  }
});

const blenderGlb = readFileSync(new URL("build/blender/AC90_board.glb", root));

check("Blender GLB is a well-formed glTF 2.0 binary", () => {
  assert.equal(blenderGlb.readUInt32LE(0), 0x46546C67, "glTF magic");
  assert.equal(blenderGlb.readUInt32LE(4), 2, "glTF version");
  assert.equal(blenderGlb.readUInt32LE(8), blenderGlb.length, "declared length");
  const jsonLen = blenderGlb.readUInt32LE(12);
  assert.equal(blenderGlb.readUInt32LE(16), 0x4E4F534A, "JSON chunk type");
  const doc = JSON.parse(
    blenderGlb.subarray(20, 20 + jsonLen).toString("utf8").trimEnd());
  assert.equal(doc.asset.version, "2.0");
  assert.equal(doc.extras.schema, "zengine.chessboard.blender/v1");
  assert.equal(doc.meshes.length, 14, "HD parts only");
  assert.equal(doc.materials.length, 11);
  assert.equal(doc.scenes[0].name, "ZEngine_AC90");
  const rootNode = doc.nodes[doc.scenes[0].nodes[0]];
  assert.equal(rootNode.name, "Artist");
  assert.equal(rootNode.children.length, 2, "SceneMesh + Sky_Dome");
  for (const mesh of doc.meshes) {
    assert.ok(mesh.primitives[0].attributes.POSITION !== undefined, mesh.name);
    assert.ok(mesh.primitives[0].indices !== undefined, mesh.name);
  }
});

check("Blender build script saves .blend and embeds AI visual params", () => {
  const script = readFileSync(
    new URL("build/blender/build_ac90.py", root), "utf8");
  for (const marker of ["import_scene.gltf", "ShaderNodeTexEnvironment",
    "CompositorNodeGlare", "save_as_mainfile", "AC90.blend", "Y-up"]) {
    assert.ok(script.includes(marker), marker);
  }
  assert.ok(!script.includes("__ZENGINE_VISUAL_JSON__"),
    "visual parameters must be injected");
  assert.ok(script.includes(visualPack.name), "AI visual pack identity");
  assert.equal(readFileSync(new URL("build/blender/sky_panorama.jpg", root)).length > 0,
    true);
});

check("Blender script imports external glb at the shared anchors", () => {
  const script = readFileSync(new URL("build/blender/build_ac90.py", root), "utf8");
  for (const marker of ["def import_external_meshes", "import_external_meshes()",
    "ZEngine_External", "hide_render = True", 'display_type = "WIRE"',
    "root.rotation_euler = (0.0, 0.0, radians)"]) {
    assert.ok(script.includes(marker), marker);
  }
  assert.ok(!script.includes("__ZENGINE_EXTERNAL_JSON__"));
  const start = script.indexOf("EXTERNAL = ");
  const end = script.indexOf("\nUSE_WORLD_SKY", start);
  assert.ok(start > 0 && end > start, "embedded EXTERNAL table");
  const embedded = JSON.parse(script.slice(start + "EXTERNAL = ".length, end));
  const external = json("build/external_mesh.manifest.json");
  assert.deepEqual(embedded.assets,
    external.assets.filter((asset) => asset.status === "asset_ready"));
  assert.deepEqual(embedded.placements,
    external.placements.filter((placement) => placement.asset_ready));
  for (const asset of embedded.assets) {
    const staged = readFileSync(new URL("build/" + asset.mesh, root));
    assert.equal(createHash("sha256").update(staged).digest("hex"),
      asset.mesh_sha256, asset.asset_id);
  }
});

{
  const external = json("build/external_mesh.manifest.json");
  const asset = external.assets.find((entry) => entry.status === "asset_ready");
  assert.ok(asset, "need an asset_ready glb");
  const bytes = readFileSync(new URL(asset.mesh, root));
  assert.equal(await sha256Hex(bytes), asset.mesh_sha256);
  assert.equal(await verifySha256(bytes, asset.mesh_sha256), true);
  const tampered = Buffer.from(bytes);
  tampered[tampered.length - 1] ^= 0xff;
  assert.equal(await verifySha256(tampered, asset.mesh_sha256), false,
    "tampered glb must be rejected");
  checks.push("web sha256 verifies glb and rejects tampered bytes");
}

check("Unity external caches keep glb indexed vertices (no triangle soup)", () => {
  // Web 端解析器也必须能吃 indexCount != vertexCount 的 ZBRD v1。
  for (const entry of stylePack.external_mesh_contract.items) {
    if (entry.status !== "asset_ready") continue;
    const { arrayBuffer } = load(entry.cache);
    const [part] = parseZBRD(arrayBuffer);
    assert.ok(part.vertexCount < part.indexCount,
      `${entry.asset_id} cache regressed to triangle soup`);
    assert.equal(part.indexCount % 3, 0);
    assert.ok(part.indices.every((i) => i < part.vertexCount),
      `${entry.asset_id} has out-of-range indices`);
  }
});

check("baseColor PNG is sha-locked and consumed by all three hosts", () => {
  const app = readFileSync(new URL("web/app.js", root), "utf8");
  const binder = readFileSync(new URL("unity/ZExternalMeshBinder.cs", root), "utf8");
  const blender = readFileSync(new URL("blender_export.py", root), "utf8");
  assert.ok(!app.includes("child.material.map = null"),
    "Web must not clear the baseColor map anymore");
  assert.ok(app.includes("THREE.SRGBColorSpace") && app.includes("flipY = false"),
    "Web must decode as sRGB with glTF V-down UVs");
  assert.ok(app.includes("asset.texture_sha256"),
    "Web must verify the texture sha256");
  assert.ok(binder.includes("ImageConversion.LoadImage"),
    "Unity must decode the shared PNG");
  assert.ok(binder.includes("1f - v"), "Unity must flip glTF V");
  assert.ok(blender.includes("EXTERNAL_TEX_SKIP"),
    "Blender template must swap in the shared PNG");
  for (const entry of stylePack.external_mesh_contract.items) {
    if (entry.status !== "asset_ready") continue;
    assert.equal(entry.texture,
      `assets/external_meshes/${entry.asset_id}.png`);
    const bytes = readFileSync(new URL(entry.texture, root));
    assert.equal(bytes.readUInt32BE(0), 0x89504e47, "must be a PNG");
    assert.equal(createHash("sha256").update(bytes).digest("hex"),
      entry.texture_sha256, entry.asset_id);
    const staged = readFileSync(new URL("build/" + entry.texture, root));
    assert.equal(staged.compare(bytes), 0,
      `${entry.asset_id} Blender staged copy must be byte-identical`);
  }
});

check("web bakes glb node TRS into geometry (no lying-down assets)", () => {
  const app = readFileSync(new URL("web/app.js", root), "utf8");
  // 混元 glb 的 nodes[0] 恒带 +90°X (Z-up -> Y-up)。若留在 mesh transform 上，
  // applyTransform 的 object.rotation.set(0, rotY, 0) 会把它整体覆盖，
  // 实测 6 个实例全部躺倒、高度只剩 world_bounds 的 40~67%。
  assert.ok(app.includes("geometry.applyMatrix4(matrix)"),
    "glb node matrix must be baked into vertex data");
  assert.ok(app.includes("matrix.compose("), "node TRS must be composed");
  assert.ok(!app.includes("mesh.quaternion.fromArray"),
    "node rotation must not be left on the mesh transform");
  assert.ok(app.includes("applyTransform(object, placement, asset.pivot_offset)"),
    "placement transform must still be applied");
});

// ---------------------------------------------------------------------------
// 两页拆分：index.html / app.js = 三端数据一致性验证；starry.html / starry.js =
// 沉浸式宣发页。两页互不改对方的行为，但共享同一批资产与同一套解析模块。
// ---------------------------------------------------------------------------

check("verification page and immersive promo page are two separate pages", () => {
  const index = readFileSync(new URL("web/index.html", root), "utf8");
  const starry = readFileSync(new URL("web/starry.html", root), "utf8");
  const app = readFileSync(new URL("web/app.js", root), "utf8");
  const css = readFileSync(new URL("web/starry.css", root), "utf8");
  assert.ok(index.includes('./app.js') && index.includes('./style.css'));
  assert.ok(!index.includes('./starry.js'), "verification page must keep its own script");
  assert.ok(index.includes('./starry.html'), "link to the immersive page");
  assert.ok(starry.includes('./starry.js') && starry.includes('./starry.css'));
  assert.ok(!starry.includes('./app.js'), "immersive page must not load the validator");
  assert.ok(starry.includes('./index.html'), "link back to the verification page");
  assert.ok(app.includes('BUILD + "external_mesh.manifest.json"')
    && app.includes('BUILD + "variants.json"'), "validator still checks LOD / variants");
  // style.css 里踩过的坑：作者 display:flex 会压过 UA 的 [hidden]，
  // 空的全屏 #error 会盖住 canvas，用户看到「无报错但不显示」。
  assert.ok(css.includes("[hidden] { display: none !important; }"));
});

check("immersive page reuses the sha-locked panorama and shared assets", () => {
  const starry = readFileSync(new URL("web/starry.js", root), "utf8");
  const panorama = readFileSync(new URL(visualPack.skybox.panorama_image, root));
  assert.equal(createHash("sha256").update(panorama).digest("hex"),
    visualPack.skybox.panorama_sha256);
  assert.ok(starry.includes("visual.skybox.panorama_image")
    && starry.includes("visual.skybox.panorama_sha256"),
  "skybox must come from visual-pack, not a new bespoke image");
  assert.ok(starry.includes('part.name === "core_slab" && part.kind === KIND.GRID'),
    "only the frozen slab outline is read from ZBRD; no sky@* sphere is built");
  assert.ok(starry.includes('BUILD + "board.zbrd"')
    && starry.includes('BUILD + "external_mesh.manifest.json"'));
  assert.ok(starry.includes("asset.cache_sha256")
    && starry.includes("asset.texture_sha256"),
  "indexed ZBRD caches and independent baseColor PNGs are both hash-verified");
  assert.ok(starry.includes("applyTransform(mesh, placement, asset.pivot_offset)"));
  assert.ok(starry.includes("UnrealBloomPass") && starry.includes("FogExp2"),
    "visual-pack bloom / fog must be consumed, not just ACES exposure");
  assert.ok(!/new\s+THREE\.GLTFLoader|loaders\/GLTFLoader\.js/.test(starry),
    "ImageBitmapLoader hangs on the embedded glb texture; keep TextureLoader");
  assert.ok(!/renderer\.useLegacyLights\s*=/.test(starry)
    && starry.includes("LUX_SCALE = Math.PI"),
  "deprecated flag; physical lights need the ×π conversion");
});

check("immersive board is painted into the terrain, not stacked as a slab", () => {
  const starry = readFileSync(new URL("web/starry.js", root), "utf8");
  const clearR = Number(/const CLEAR_R = (\d+);/.exec(starry)?.[1]);
  assert.ok(Number.isFinite(clearR) && clearR >= 14,
    "clear centre must fit the board region plus its outer decorations (~11.4 m)");
  // 用户实测纠正：现网棋盘（4.png）与地面共面，靠场地线 / 中圈徽记划出区域；
  // 不能把 ZBRD 台面当独立方块盖在地形上。
  assert.ok(starry.includes("convexHullXZ") && starry.includes("paintBoardBase")
    && starry.includes("paintBoardLines"),
  "board outline from ZBRD must be drawn into the ground canvas texture");
  assert.ok(starry.includes("makeGroundTexture(board)")
    && starry.includes("buildGround(state.board)"));
  assert.ok(starry.indexOf("state.board = await loadBoardRegion()")
    < starry.indexOf("buildGround(state.board)"),
  "outline must be parsed before the terrain texture is generated");
  assert.ok(!starry.includes("new THREE.Mesh(makeGeometry(part), material)"),
    "no standalone ZBRD slab / frame / anchor meshes may be added to the scene");
  assert.ok(!/islandGroup\.position\.y = -0\.\d+;/.test(starry),
    "no z-fighting workaround needed once the slab is gone");
  assert.ok(!starry.includes("r < 0.30 && random() < 0.94"),
    "brush strokes must keep flowing through the board region");
  assert.ok(starry.includes("CLEAR_R + 3") && starry.includes("CLEAR_R - 2"),
    "trees / brush strokes must be placed outside the clear centre");
});

check("board core is one square plus two diagonal player seats", () => {
  const core = spec.core_contract;
  assert.deepEqual(core.required_elements, ["square_outline", "player_seats"]);
  assert.deepEqual(core.square_outline.fixed_parts, ["core_slab", "frame_*"]);
  assert.equal(core.player_seats.count, 2);
  assert.deepEqual(core.player_seats.corners, ["south_west", "north_east"]);
  const seats = spec.anchors.items;
  assert.equal(seats.length, 2);
  assert.deepEqual(seats.map((s) => s.corner).sort(),
    ["north_east", "south_west"]);
  const [a, b] = seats.map((s) => s.position);
  assert.ok(a[0] * b[0] < 0 && a[2] * b[2] < 0,
    "the two player seats must sit on opposite corners");
  const reserved = spec.decoration_zones.items.filter((z) => z.reserved_by);
  assert.equal(reserved.length, 2);
  assert.ok(reserved.every((z) => z.max_items === 0));
  assert.equal(manifest.anchors.length, 2);
  assert.deepEqual(
    parts.filter((p) => p.name.startsWith("anchor_")).map((p) => p.name).sort(),
    ["anchor_seat_guest", "anchor_seat_home"]);
  assert.ok(!parts.some((p) => p.name.includes("info_plinth")),
    "the third plinth is no longer a frozen core element");
});

check("concept sketch marks are optional, never frozen", () => {
  const starry = readFileSync(new URL("web/starry.js", root), "utf8");
  const worlds = json("web/worlds.json");
  assert.deepEqual(worlds.defaults.board.marks, []);
  for (const world of worlds.worlds) {
    assert.equal(world.overrides?.board?.marks, undefined,
      `${world.id} must not force sketch lines by default`);
  }
  assert.ok(worlds.contract.board_required_elements.includes("marks"));
  assert.ok(starry.includes("const marks = boardSkin.marks ?? []"));
  for (const mark of ["center_line", "half_circles", "center_emblem"]) {
    assert.ok(starry.includes(`marks.includes("${mark}")`),
      `${mark} must be opt-in from the concept art`);
  }
  assert.ok(!starry.includes("八芒星") && !starry.includes("setLineDash"),
    "sketch-only ornaments (8-point star / dashed logical bounds) are gone");
});

check("drag uses absolute deltas and scenery replaces board decorations", () => {
  const starry = readFileSync(new URL("web/starry.js", root), "utf8");
  // 用户实测「轻微拖拽旋转力度很大」：旧代码把「相对按下点的累积 dx」在每个
  // pointermove 里重复累减一次角度，拖 40px / 8 个事件就转出几十度；
  // dragging.moved 也被同样累加，点击判定随之失效。
  assert.ok(!/^\s*cam\.azimuth -= dx/m.test(starry),
    "azimuth must not accumulate the same delta on every pointermove");
  assert.ok(starry.includes("azimuth: cam.azimuth, elevation: cam.elevation")
    && starry.includes("dragging.azimuth - (dx / innerWidth)"),
  "drag must map from the press-time angle with viewport-normalised delta");
  assert.ok(starry.includes("dragging.moved = Math.max"),
    "click threshold must use max displacement, not summed deltas");
  // 用户实测：围在棋盘四周的金色装饰物与星夜场景不匹配，改用场景物件。
  assert.equal(/const USE_EXTERNAL_PROPS = (true|false);/.exec(starry)?.[1], "false");
  assert.ok(starry.includes("function buildSceneProps()")
    && starry.includes("buildSceneProps();"));
  assert.ok(starry.includes("scenePropsGroup.add(group)"),
    "street lanterns belong to the scenery, not to board anchors");
  assert.ok(starry.includes("data.hint"),
    "inspect panel must describe scenery, not asset blueprint metadata");
});

// ---------------------------------------------------------------------------
// 12 画境视觉包：只准覆盖「颜色 / 天空 / 物件 / 灯光」，几何永远来自同一份
// ZBRD 轮廓。无头探针只能证明页面能跑，这些断言锁住数据契约本身。
// ---------------------------------------------------------------------------

const worldDoc = json("web/worlds.json");
const starrySource = readFileSync(new URL("web/starry.js", root), "utf8");
const skySource = readFileSync(new URL("web/world-sky.mjs", root), "utf8");
const scenerySource = readFileSync(new URL("web/world-scenery.mjs", root), "utf8");
const worldPackSource = readFileSync(new URL("web/world-pack.mjs", root), "utf8");

check("12 world packs are visual overlays, never a second geometry source", () => {
  assert.equal(worldDoc.schema, WORLD_PACK_SCHEMA);
  assert.equal(worldDoc.source.count, 12);
  assert.equal(worldDoc.worlds.length, 12);
  assert.ok(worldDoc.worlds.some((world) => world.id === worldDoc.default),
    "default must name a real world");
  assert.equal(new Set(worldDoc.worlds.map((world) => world.id)).size, 12);
  assert.deepEqual(worldDoc.worlds.map((world) => world.index),
    Array.from({ length: 12 }, (_, i) => i + 1));
  // 轮廓 / 锚点 / 逻辑边界只在 board.zbrd + board.manifest.json 里定。
  const visualLayers = new Set(["sky", "ground", "board", "scenery", "lighting"]);
  for (const world of worldDoc.worlds) {
    assert.ok(["high", "tone"].includes(world.tier), world.id);
    assert.ok(world.title_zh && world.title_en && world.artist
      && world.year && world.description, `${world.id} metadata`);
    for (const key of Object.keys(world.overrides)) {
      assert.ok(visualLayers.has(key), `${world.id} overrides geometry layer ${key}`);
    }
    for (const key of ["eyebrow", "sub", "from", "footer"]) {
      assert.ok(world.hud[key], `${world.id}.hud.${key}`);
    }
    assert.ok(world.concept.prompt.length > 40, `${world.id} needs a real prompt`);
    assert.ok(world.concept.image, `${world.id}.concept.image`);
  }
  assert.ok(worldDoc.contract.board_is_terrain.includes("不是独立 3D 台面"));
  assert.deepEqual(worldDoc.contract.sky_modes,
    ["panorama_sha_locked", "procedural_canvas"]);
  assert.ok(worldDoc.note.includes("build/board.zbrd"));
});

check("world packs deep-merge defaults and keep 12 distinct palettes", () => {
  const merged = mergeWorld(
    { strokes: ["a", "b"], sky: { top: "#000000", fog: { density: 1 } } },
    { strokes: ["c"], sky: { horizon: "#ffffff", fog: { color: "#abcdef" } } });
  assert.deepEqual(merged.strokes, ["c"], "arrays replace, never append");
  assert.equal(merged.sky.top, "#000000");
  assert.equal(merged.sky.fog.density, 1);
  assert.equal(merged.sky.fog.color, "#abcdef");

  const packs = worldDoc.worlds.map((world) => resolveWorldPack(worldDoc, world.id));
  assert.equal(resolveWorldPack(worldDoc, "not-a-world").id, worldDoc.default,
    "unknown ?world= must fall back to the default");
  const fingerprints = new Set();
  for (const pack of packs) {
    assert.equal(pack.overrides, undefined, `${pack.id} leaked overrides`);
    for (const key of ["sky", "ground", "board", "scenery", "lighting"]) {
      assert.ok(pack[key] && Object.keys(pack[key]).length > 0, `${pack.id}.${key}`);
    }
    assert.equal(pack.sky.mode,
      pack.id === "starry-night" ? "panorama_sha_locked" : "procedural_canvas",
      `${pack.id} sky mode`);
    assert.ok(pack.sky.top.startsWith("#") && pack.sky.horizon.startsWith("#"));
    assert.ok(Number.isFinite(pack.sky.fog.density) && Number.isFinite(pack.sky.exposure));
    assert.ok(Number.isFinite(pack.sky.bloom.threshold));
    assert.equal(pack.ground.gradient.length, 3, `${pack.id} gradient`);
    assert.equal(pack.ground.strokes.length, 7, `${pack.id} strokes`);
    assert.equal(pack.board.line.length, 3, `${pack.id} board line`);
    assert.ok(pack.scenery.props.hint && pack.scenery.particles.dust, `${pack.id} scenery`);
    assert.ok(Array.isArray(pack.lighting.hemisphere), `${pack.id} hemisphere`);
    fingerprints.add(JSON.stringify([pack.ground.gradient, pack.ground.strokes,
      pack.board.line, pack.sky.top]));
  }
  assert.equal(fingerprints.size, 12, "each world needs its own palette");
  assert.equal(packs[0].sky.panorama_sha256, visualPack.skybox.panorama_sha256);
});

check("all 12 worlds are high fidelity with their own scenery preset", () => {
  const high = worldDoc.worlds.filter((world) => world.tier === "high");
  assert.equal(high.length, 12, "no tone worlds left");
  const expected = {
    "starry-night": "starry", "cafe-terrace": "cafe",
    "wheatfield-cypresses": "wheat", "bedroom-arles": "bedroom",
    "impression-sunrise": "sunrise", "japanese-footbridge": "pond",
    "garden-sainte-adresse": "garden", "bridge-argenteuil": "argenteuil",
    "villeneuve-bridge": "villeneuve", "port-marly-flood": "marly",
    "mont-sainte-victoire": "victoire", "gauguin-path": "gauguin",
  };
  for (const world of worldDoc.worlds) {
    assert.equal(resolveWorldPack(worldDoc, world.id).scenery.preset,
      expected[world.id], world.id);
  }
  const declared = /const PRESETS = \{([\s\S]*?)\n\};/.exec(scenerySource)?.[1] ?? "";
  for (const preset of new Set(Object.values(expected))) {
    if (preset === "starry") continue; // 内建于 starry.js 的树丛 / 村庄
    assert.ok(declared.includes(preset + ":"), `${preset} preset missing`);
  }
  assert.equal(new Set(Object.values(expected)).size, 12,
    "no two worlds may share a preset");
  assert.ok(scenerySource.includes("export const SCENERY_PRESETS"));
  assert.ok(scenerySource.includes("InstancedMesh") && scenerySource.includes("CLEAR_R"),
    "presets stay procedural and leave the painted board region clear");
  assert.ok(scenerySource.includes("CAMERA_ORBIT_R")
    && scenerySource.includes("CAMERA_CLEARANCE"),
  "scenery must stay off the r=32 camera orbit or a tree fills the screen");
  assert.ok(starrySource.includes('pack.scenery.preset === "generic"')
    && starrySource.includes("buildWorldScenery(pack.scenery.preset"),
  "page must dispatch on the pack's preset");
});

check("all 12 concept arts are sha-locked AI assets", () => {
  const manifest = json("assets/worlds/manifest.json");
  assert.equal(manifest.schema, "zengine.chessboard.world-concept/v1");
  assert.equal(manifest.count, 11);
  assert.equal(manifest.items.length, 11);
  const digests = new Set();
  for (const world of worldDoc.worlds) {
    assert.equal(world.concept.status, "asset_ready", world.id);
    const bytes = readFileSync(new URL(world.concept.image, root));
    const digest = createHash("sha256").update(bytes).digest("hex");
    assert.equal(digest, world.concept.sha256, world.id);
    digests.add(digest);
    if (world.id === "starry-night") {
      assert.equal(digest, visualPack.concept.sha256);
      assert.equal(bytes.readUInt16BE(0), 0xffd8, "starry concept is JPEG");
      continue;
    }
    assert.equal(bytes.readUInt32BE(0), 0x89504e47, `${world.id} must be PNG`);
    const entry = manifest.items.find((item) => item.id === world.id);
    assert.ok(entry, `${world.id} missing from concept manifest`);
    assert.equal(entry.image, world.concept.image);
    assert.equal(entry.sha256, digest);
    assert.deepEqual(entry.size_px,
      [bytes.readUInt32BE(16), bytes.readUInt32BE(20)], world.id);
    assert.ok(entry.size_px[0] >= 512 && entry.size_px[1] >= 512);
    assert.equal(entry.prompt, world.concept.prompt);
  }
  // 并发生成时曾出现「不同 prompt 拿到同一张图」，所以 12 个 sha 必须互异。
  assert.equal(digests.size, 12, "each world needs its own concept art");
  // prompt_only 降级分支仍保留，未来缺图时不会留死链。
  assert.ok(worldPackSource.includes('pack.concept.status === "asset_ready"')
    && worldPackSource.includes("skyPreviewURL(pack)")
    && worldPackSource.includes('image.src = skyPreviewURL(pack)'));
});

check("promo page switches 12 worlds by URL without rebuilding ZBRD geometry", () => {
  const html = readFileSync(new URL("web/starry.html", root), "utf8");
  const css = readFileSync(new URL("web/starry.css", root), "utf8");
  const serve = readFileSync(new URL("tools/serve.py", root), "utf8");
  assert.ok(html.includes('id="world-select"'));
  for (const id of ["loading-sub", "hud-eyebrow", "hud-title", "hud-sub",
    "hud-from", "credits-line", "thumb"]) {
    assert.ok(html.includes(`id="${id}"`), id);
  }
  assert.ok(css.includes("#world-select"));
  assert.ok(starrySource.includes('fetchJSON("./worlds.json")')
    && starrySource.includes("applyWorldHud(worldsDoc, pack, $)"));
  assert.ok(starrySource.includes("url.searchParams.set(\"world\"")
    && starrySource.includes("location.assign"),
  "selector / URL are the only world-switch entry points");
  assert.ok(starrySource.includes("state.board = await loadBoardRegion()")
    && starrySource.includes("buildGround(state.board)"),
  "the same frozen ZBRD outline is parsed first for every world");
  assert.ok(!starrySource.includes("new THREE.Mesh(makeGeometry(part), material)"),
    "no world may bring back a standalone slab");
  // 白昼世界关掉星空后，加色浮尘也必须跟着降透明度并直接隐藏，否则满屏白点。
  assert.ok(starrySource.includes("dustOpacity = 0.25 + 0.55 * state.skin.sky.stars")
    && starrySource.includes("dust.visible = state.skin.sky.stars > 0.05"));
  for (const extension of [".mjs", ".jpg", ".jpeg", ".png"]) {
    assert.ok(serve.includes(`"${extension}"`), `${extension} MIME`);
  }
});

check("procedural sky compensates sphere UV stretch and stays sRGB", () => {
  // 低仰角相机只看到贴图下半部；太阳/云必须在画布里预先反向压扁，
  // 否则贴在球面上会被拉成横椭圆，地平线也会出现硬断层。
  assert.ok(skySource.includes("const SUN_X_SQUEEZE")
    && skySource.includes("ctx.scale(SUN_X_SQUEEZE, 1)"));
  assert.ok(skySource.includes("ctx.scale(rx / ry, 1)")
    && skySource.includes("createRadialGradient(0, 0, 0, 0, 0, ry)"),
  "clouds must be filled soft blobs, not stroked ellipse outlines");
  assert.ok(!skySource.includes("ctx.ellipse(x + offset, y, rx, ry"),
    "stroked cloud outlines look like soap bubbles on the sphere");
  assert.ok(skySource.includes("gradient.addColorStop(0.72"));
  assert.ok(skySource.includes("return `#${channel(0)}"),
    "mix() must return #rrggbb: rgba() only parses hex");
  assert.ok(skySource.includes("canvas.width = 2048")
    && skySource.includes("canvas.height = 1024"));
  assert.ok(skySource.includes("texture.colorSpace = THREE.SRGBColorSpace")
    && skySource.includes("texture.wrapS = THREE.RepeatWrapping"));
  assert.ok(starrySource.includes("makeSkyPanorama(THREE, pack)"));
});

check("GitHub Pages build packages every local .mjs module", () => {
  const script = readFileSync(new URL("tools/build_pages.py", root), "utf8");
  const imported = [starrySource, scenerySource, worldPackSource]
    .flatMap((source) => [...source.matchAll(/from "\.\/([\w-]+\.mjs)"/g)]
      .map((match) => match[1]));
  for (const name of new Set(imported)) {
    assert.ok(script.includes(`"${name}"`), `build_pages.py 漏了 ${name}`);
  }
  // Pages 对 .mjs 的 MIME 不稳定，ES module 会被严格 MIME 检查拒掉。
  assert.ok(script.includes("LOCAL_MODULE") && script.includes('.replace(".mjs", ".js")'),
    "local modules must be copied as .js");
  assert.ok(script.includes(".nojekyll") && script.includes("port-marly-flood"));
});

check("board ceremony freezes 6 host-dispatched events", () => {
  const doc = json("ceremony.json");
  const module = readFileSync(new URL("web/board-ceremony.mjs", root), "utf8");
  const html = readFileSync(new URL("web/starry.html", root), "utf8");
  const css = readFileSync(new URL("web/starry.css", root), "utf8");
  const worlds = json("web/worlds.json");
  const script = readFileSync(new URL("tools/build_pages.py", root), "utf8");
  const ids = ["entrance", "round_win", "round_lose",
    "win_streak", "level_up", "easter_egg"];
  assert.equal(doc.schema, "zengine.chessboard.ceremony/v1");
  assert.equal(doc.frozen, true);
  assert.deepEqual(doc.events.map((event) => event.id), ids);
  assert.ok(doc.transport.rule.includes("不得自行推断"));
  assert.ok(!/streak\s*\+\+|level\s*\+\+/.test(module),
    "棋盘不得自己累加连胜 / 等级");
  for (const event of doc.events) {
    assert.ok(event.duration_ms > 0 && event.label_zh.length > 0);
    assert.ok(module.includes(`case "${event.id}"`), `${event.id} 缺少横幅文案`);
    assert.ok(html.includes(`value="${event.id}"`), `${event.id} 缺少控件入口`);
  }
  assert.deepEqual(worlds.defaults.scenery.ceremony, doc.palette_defaults);
  assert.ok(worlds.contract.board_ceremony.includes("宿主"));
  assert.ok(starrySource.includes("createCeremony")
    && starrySource.includes('.get("event")')
    && starrySource.includes("GROUND_PLANE")
    && starrySource.includes("payload.auto"));
  assert.ok(css.includes("#ceremony") && css.includes("pointer-events: none"),
    "演出横幅不能吞掉 canvas 手势");
  assert.ok(script.includes('"board-ceremony.mjs"')
    && script.includes('"ceremony.json"') && script.includes("&event="));
});

check("entrance shots derive from registered scenery landmarks", () => {
  // 1. 11 个 preset 在生成几何时登记地标，且坐标与几何同源。
  const presets = ["wheat", "pond", "sunrise", "cafe", "bedroom", "garden",
    "argenteuil", "villeneuve", "marly", "victoire", "gauguin"];
  assert.ok(scenerySource.includes("function feature(")
    && scenerySource.includes("ctx.features ??= []"),
  "world-scenery.mjs 要有地标登记钩子");
  assert.equal((scenerySource.match(/feature\(ctx, "/g) ?? []).length,
    presets.length * 3, "11 个 preset 各登记 3 处地标");
  for (const preset of presets) {
    assert.ok(scenerySource.includes(`"${preset}_`),
      `${preset} 没有登记任何地标`);
  }
  for (const source of ["[0, 2, bridgeZ]", "[0, 3.3, bridgeZ]",
    "[bridgeX, 4, 0]", "[tables[0].x", "[awnings[0].x", "[flags[0].x",
    "[palms[0].x", "ISLAND_R - 14", "ISLAND_R - 2", "strandCentres[0]"]) {
    assert.ok(scenerySource.includes(source),
      "地标焦点必须与几何共用变量: " + source);
  }
  // 2. starry 内建画境同样登记，features 汇入同一份数组。
  assert.ok(starrySource.includes("features: []")
    && starrySource.includes("features: state.features")
    && starrySource.includes("registerStarryFeatures(buildFoliage())")
    && starrySource.includes("return { houses, cypressSites }")
    && starrySource.includes("function registerFeature("),
  "starry 内建画境也要把几何锚点登记进 state.features");
  // 3. 镜头由登记派生，不再有手写的 12 组镜像表。
  assert.ok(!starrySource.includes("ENTRANCE_SHOTS")
    && !starrySource.includes("ENTRANCE_START"),
  "不能再维护第二份手写分镜坐标表");
  assert.ok(starrySource.includes("const SHOT_LANGUAGE")
    && starrySource.includes("function deriveEntranceFrames")
    && starrySource.includes("function languageOf")
    && starrySource.includes("state.features ?? []")
    && starrySource.includes("state.board?.outline")
    && starrySource.includes("subject: lang.subject")
    && starrySource.includes("b.weight ?? 1) - (a.weight ?? 1)"),
  "分镜按 kind 查机位语言、按 weight 选站点、从棋盘轮廓派生棋盘站");
  const kinds = new Set([...starrySource.matchAll(/subject: "([^"]+)"/g)]
    .map((m) => m[1]));
  assert.ok(kinds.has("建筑") && kinds.has("河流") && kinds.has("棋盘")
    && kinds.has("山峰") && kinds.has("桥"),
  "机位语言要覆盖建筑物 / 河流 / 棋盘 / 山峰 / 桥");
  // 4. 前几轮踩过的坑继续钉死。
  assert.ok(starrySource.includes("const s = u * u * (3 - 2 * u)")
    && !starrySource.includes("function catmull("),
  "帧间必须用不会过冲的分段 smoothstep");
  assert.ok(starrySource.includes("cam.azimuth = frame.az * DEG")
    && starrySource.includes("az: home.azimuth / DEG"),
  "分镜表用度、cam.azimuth 用弧度，两处都必须显式换算");
  assert.ok(starrySource.includes("THREE.MathUtils.clamp(mainLang.arc * 2.4, 30, 54)"),
  "短弧幅度由地标机位语言决定且不超过 54°");
  assert.ok(starrySource.includes("sampleEntranceFrame")
    && starrySource.includes("makeEntranceFrames")
    && starrySource.includes("cam.panX = frame.focus[0]")
    && starrySource.includes("startEntranceOrbit")
    && starrySource.includes("settleEntrance")
    && starrySource.includes("entranceShot?.auto"),
  "分镜插值 / 落位 / 探针基线都要接上");
  assert.ok(starrySource.includes("buildEntranceFx")
    && starrySource.includes("state.entranceLights")
    && starrySource.includes("ceremony?.stage?.("),
  "方格亮起 / 灯光渐亮 / 分段点名特色都要接上");
  const module = readFileSync(new URL("web/board-ceremony.mjs", root), "utf8");
  assert.ok(module.includes("function stage(")
    && module.includes("dispatch, stop, stage"),
  "横幅要提供 stage() 分站点文案");
  const css = readFileSync(new URL("web/starry.css", root), "utf8");
  assert.ok(css.includes("ceremonyStage")
    && css.includes('data-event="entrance"'),
  "入场横幅需要分站刻度与淡入过渡");
});

check("mobile portrait blocks pull-to-refresh and keeps HUD usable", () => {
  const css = readFileSync(new URL("web/starry.css", root), "utf8");
  const html = readFileSync(new URL("web/starry.html", root), "utf8");
  assert.ok(css.includes("overscroll-behavior: none")
    && css.includes("touch-action: none"),
  "touch gestures must not scroll / refresh the whole page");
  assert.ok(css.includes("orientation: portrait")
    && css.includes("env(safe-area-inset-bottom)"),
  "portrait HUD needs a bottom bar clear of the home indicator");
  assert.ok(html.includes("viewport-fit=cover") && html.includes("maximum-scale=1"));
  assert.ok(starrySource.includes("pointercancel")
    && starrySource.includes('"touchmove"')
    && starrySource.includes("{ passive: false }"),
  "canvas touchmove must be non-passive; cancel must clear dragging");
  assert.ok(starrySource.includes("viewportTuning")
    && starrySource.includes("tunedPreset")
    && starrySource.includes("tuning.key !== lastTuneKey"),
  "portrait needs wider fov / farther camera, reapplied on rotation");
  assert.ok(starrySource.includes("pinchDistance")
    && starrySource.includes("event.isPrimary"),
  "touch devices have no wheel: pinch zooms, only the primary pointer drags");
});

console.log(`ZBRD JS cross-check: ${checks.length} checks / 0 failures`);
for (const name of checks) console.log(`  ok  ${name}`);
