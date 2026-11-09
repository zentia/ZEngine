// glb + sha256 + 锚点的 Web 消费端（纯逻辑，不依赖 three.js）。
//
// 真源是 external_mesh.py 产出的 external_mesh.manifest.json：
//   world = anchor + rotY(rotation_y_deg) * scale * (local - pivot_offset)
// 这里只做三件事：sha256 校验、glb 头校验、把锚点换算成 three.js 变换。
// 与 Unity / Blender 用同一份清单、同一条公式，Node 侧可直接测。

export const EXTERNAL_MANIFEST_SCHEMA =
  "zengine.chessboard.external-mesh-manifest/v1";
export const GLB_MAGIC = 0x46546c67;

export async function sha256Hex(bytes) {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const digest = await globalThis.crypto.subtle.digest("SHA-256", view);
  return [...new Uint8Array(digest)]
    .map((value) => value.toString(16).padStart(2, "0")).join("");
}

export async function verifySha256(bytes, expected) {
  if (typeof expected !== "string" || expected.length !== 64) return false;
  return (await sha256Hex(bytes)) === expected.toLowerCase();
}

export function readGlbHeader(bytes) {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (view.byteLength < 12) throw new Error("glb too short");
  const header = new DataView(view.buffer, view.byteOffset, 12);
  if (header.getUint32(0, true) !== GLB_MAGIC) throw new Error("bad glb magic");
  return {
    version: header.getUint32(4, true),
    byteLength: header.getUint32(8, true),
    actualLength: view.byteLength,
  };
}

export function placementTransform(placement, pivot = [0, 0, 0]) {
  const anchor = placement.anchor ?? [0, 0, 0];
  const scale = Number(placement.scale ?? 1);
  const radians = (Number(placement.rotation_y_deg ?? 0) * Math.PI) / 180;
  const cos = Math.cos(radians);
  const sin = Math.sin(radians);
  const offset = [
    Number(pivot[0] ?? 0) * scale,
    Number(pivot[1] ?? 0) * scale,
    Number(pivot[2] ?? 0) * scale,
  ];
  const dx = offset[0] * cos + offset[2] * sin;
  const dz = -offset[0] * sin + offset[2] * cos;
  return {
    position: [anchor[0] - dx, anchor[1] - offset[1], anchor[2] - dz],
    rotationY: radians,
    scale,
  };
}

export function applyTransform(object, placement, pivot = [0, 0, 0]) {
  const transform = placementTransform(placement, pivot);
  object.position.set(...transform.position);
  object.rotation.set(0, transform.rotationY, 0);
  object.scale.setScalar(transform.scale);
  return transform;
}

export function transformedBounds(bounds, transform) {
  const lo = bounds.min;
  const hi = bounds.max;
  const cos = Math.cos(transform.rotationY);
  const sin = Math.sin(transform.rotationY);
  const points = [];
  for (const x of [lo[0], hi[0]]) {
    for (const y of [lo[1], hi[1]]) {
      for (const z of [lo[2], hi[2]]) {
        const sx = x * transform.scale;
        const sy = y * transform.scale;
        const sz = z * transform.scale;
        points.push([
          transform.position[0] + sx * cos + sz * sin,
          transform.position[1] + sy,
          transform.position[2] - sx * sin + sz * cos,
        ]);
      }
    }
  }
  return {
    min: [0, 1, 2].map((axis) => Math.min(...points.map((p) => p[axis]))),
    max: [0, 1, 2].map((axis) => Math.max(...points.map((p) => p[axis]))),
  };
}

export function groupPlacements(manifest) {
  const assets = new Map((manifest.assets ?? []).map((a) => [a.asset_id, a]));
  const groups = new Map();
  for (const placement of manifest.placements ?? []) {
    if (!placement.asset_ready) continue;
    const asset = assets.get(placement.asset_id);
    if (!asset || asset.status !== "asset_ready") continue;
    if (!groups.has(placement.asset_id)) groups.set(placement.asset_id, []);
    groups.get(placement.asset_id).push(placement);
  }
  return { assets, groups };
}
