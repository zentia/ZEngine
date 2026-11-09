// 13 个高完成度画境里，除 starry（沿用 starry.js 已有的树丛 / 柏树 / 村庄）
// 之外的 12 套专属场景物件。全部是 InstancedMesh / 程序化几何，不引入任何
// 外部模型或贴图；颜色取自 worlds.json 的 scenery / ground 调色板。
//
// 与棋盘的关系不变：所有物件都落在 CLEAR_R 之外，中央那块「画进地形的棋盘
// 区域」保持空旷，绝不把装饰物围在棋盘四周。

import * as THREE from "three";
import { KIND } from "./zbrd.js";

// 沉浸式相机在半径 32 的轨道上环视中心；若把一根 3m 的树干或 10m 的柏树
// 撒在轨道附近，它会贴着镜头占掉半个画面（麦田页实测出现过棕色巨柱）。
// 专属物件一律避开这条轨道带，用户拖到任何 azimuth 都不会贴脸。
const CAMERA_ORBIT_R = 32;
const CAMERA_CLEARANCE = 8;

// 场景特色登记：每个 preset 在生成地标时把「镜头要看的东西」记下来。focus
// 与几何共用同一批变量 / 数组（bridgeZ、bridgeX、palms、flags、beds ...），
// 镜头不再另写一份镜像坐标。kind 决定机位语言（桥 / 水面 / 建筑 / 山峰），
// weight 是入场选站优先级，shot 可覆盖该 kind 的默认机位参数。
function feature(ctx, id, label, kind, focus, extra = {}) {
  ctx.features.push({ id, label, kind, focus, weight: 1, ...extra });
}

function annulus(random, clearR, islandR, pad = 2) {
  let radius = 0;
  for (let attempt = 0; attempt < 10; attempt += 1) {
    radius = clearR + pad + random() * (islandR - clearR - pad - 4);
    if (Math.abs(radius - CAMERA_ORBIT_R) > CAMERA_CLEARANCE) break;
  }
  if (Math.abs(radius - CAMERA_ORBIT_R) <= CAMERA_CLEARANCE) {
    radius = CAMERA_ORBIT_R - CAMERA_CLEARANCE - 2;
  }
  const angle = random() * Math.PI * 2;
  return { angle, radius, x: Math.cos(angle) * radius, z: Math.sin(angle) * radius };
}

function instanced(group, geometry, material, count, fill) {
  const mesh = new THREE.InstancedMesh(geometry, material, count);
  const dummy = new THREE.Object3D();
  for (let i = 0; i < count; i += 1) {
    // 每个实例都从单位变换开始：不给 fill 留「上一个实例的残留旋转/缩放」。
    dummy.position.set(0, 0, 0);
    dummy.rotation.set(0, 0, 0);
    dummy.scale.set(1, 1, 1);
    fill(dummy, i, mesh);
    dummy.updateMatrix();
    mesh.setMatrixAt(i, dummy.matrix);
  }
  mesh.instanceMatrix.needsUpdate = true;
  if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
  mesh.frustumCulled = false;
  group.add(mesh);
  return mesh;
}

function tones(list) {
  return list.map((hex) => new THREE.Color(hex));
}

function flatMaterial(extra = {}) {
  return new THREE.MeshStandardMaterial({
    color: 0xffffff, flatShading: true, roughness: 0.96, metalness: 0, ...extra,
  });
}

function buildWheat(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const gold = tones(skin.ground.strokes);
  const stalkCount = 1500;
  instanced(group, new THREE.ConeGeometry(0.055, 0.9, 3), flatMaterial(),
    stalkCount, (dummy, i, mesh) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 2);
      dummy.position.set(p.x, 0.46, p.z);
      dummy.rotation.set((random() - 0.5) * 0.32, random() * Math.PI,
        (random() - 0.5) * 0.32);
      const s = 0.7 + random() * 0.9;
      dummy.scale.set(s, s * (0.9 + random() * 0.7), s);
      mesh.setColorAt(i, gold[Math.floor(random() * gold.length)]);
    });

  // 柏树：麦田里近乎黑色的尖塔剪影。
  instanced(group, new THREE.ConeGeometry(1, 1, 7),
    flatMaterial({ color: skin.scenery.cypress, emissive: skin.scenery.cypress_emissive,
      emissiveIntensity: 0.25 }), 15, (dummy) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 7);
      const height = 9 + random() * 9;
      dummy.position.set(p.x, height / 2, p.z);
      dummy.rotation.set(0, random() * Math.PI, 0);
      dummy.scale.set(1.0 + random() * 0.8, height, 1.0 + random() * 0.8);
    });

  // 银绿橄榄树：低面数球团 + 短树干。树冠和树干必须共用同一份位置表，
  // 否则球团会悬在半空（实测挡住棋盘区域的巨大绿球就是这么来的）。
  const olives = tones(skin.scenery.foliage_tones);
  const oliveTrees = Array.from({ length: 14 }, (_, i) => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 4),
    height: 3.0 + (i % 4) * 0.35,
  }));
  instanced(group, new THREE.IcosahedronGeometry(1, 1), flatMaterial(),
    14 * 3, (dummy, i, mesh) => {
      const tree = oliveTrees[Math.floor(i / 3)];
      dummy.position.set(tree.x + (random() - 0.5) * 1.2,
        tree.height + (random() - 0.5) * 0.5, tree.z + (random() - 0.5) * 1.2);
      dummy.rotation.set(random(), random() * Math.PI, random());
      const r = 0.75 + random() * 0.5;
      dummy.scale.set(r, r * 0.8, r);
      mesh.setColorAt(i, olives[Math.floor(random() * olives.length)]);
    });
  instanced(group, new THREE.CylinderGeometry(0.16, 0.26, 3.2, 6),
    flatMaterial({ color: skin.scenery.trunk }), 14, (dummy, i) => {
      const tree = oliveTrees[i];
      dummy.position.set(tree.x, 1.6, tree.z);
      dummy.rotation.set(0, 0, 0);
      dummy.scale.set(1, 1, 1);
    });

  // 麦捆与田埂木桩，属于「场景物件」而不是棋盘摆件。
  instanced(group, new THREE.CylinderGeometry(1.5, 1.7, 1.15, 10),
    flatMaterial({ color: skin.ground.path_base }), 12, (dummy) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 5);
      dummy.position.set(p.x, 0.58, p.z);
      dummy.rotation.set(0, random() * Math.PI, 0);
      dummy.scale.set(0.8 + random() * 0.5, 1, 0.8 + random() * 0.5);
    });
  instanced(group, new THREE.BoxGeometry(0.14, 0.9, 0.14),
    flatMaterial({ color: skin.scenery.trunk }), 72, (dummy) => {
      const angle = (Math.floor(random() * 72) / 72) * Math.PI * 2;
      const radius = ISLAND_R - 5 + random() * 2;
      dummy.position.set(Math.cos(angle) * radius, 0.45, Math.sin(angle) * radius);
      dummy.rotation.set(0, angle, (random() - 0.5) * 0.18);
      dummy.scale.set(1, 0.8 + random() * 0.5, 1);
    });

  const glint = new THREE.PlaneGeometry(1, 1);
  glint.rotateX(-Math.PI / 2);
  instanced(group, glint, new THREE.MeshBasicMaterial({
    color: skin.ground.white_stroke, transparent: true, opacity: 0.30,
    side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
  }), 300, (dummy) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 1);
    dummy.position.set(p.x, 0.06, p.z);
    dummy.rotation.set(0, random() * Math.PI, 0);
    dummy.scale.set(0.4 + random() * 1.8, 1, 0.06 + random() * 0.14);
  });

  // 可点击的界石：inspect 用场景化描述。
  for (let i = 0; i < 3; i += 1) {
    const p = annulus(random, CLEAR_R, ISLAND_R, 8);
    const stone = new THREE.Mesh(
      new THREE.CylinderGeometry(0.34, 0.42, 0.8, 6),
      flatMaterial({ color: skin.scenery.props.stone }));
    stone.position.set(p.x, 0.4, p.z);
    stone.userData = {
      id: `${skin.scenery.props.id_prefix}_${i}`, tag: skin.scenery.props.tag,
      hint: skin.scenery.props.hint, kind: KIND.INTERACTABLE,
    };
    group.add(stone);
    ctx.interactive.push(stone);
  }
  // 麦田环带 CLEAR_R..ISLAND_R 上的代表锚点；橄榄树用同一批生成锚点。
  feature(ctx, "wheat_field", "金色麦浪", "field",
    [-(CLEAR_R + 6), 2, CLEAR_R - 6], { weight: 2 });
  feature(ctx, "wheat_olive", "银绿橄榄树", "grove",
    [oliveTrees[0].x, oliveTrees[0].height, oliveTrees[0].z], { weight: 2 });
  feature(ctx, "wheat_cypress", "柏树尖塔", "grove",
    [-(CLEAR_R + 10), 9, -(CLEAR_R + 4)], { weight: 3 });
}

function buildPond(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const greens = tones(skin.scenery.foliage_tones);
  const padGeometry = new THREE.CircleGeometry(1, 12);
  padGeometry.rotateX(-Math.PI / 2);
  instanced(group, padGeometry, new THREE.MeshBasicMaterial({
    color: 0xffffff, side: THREE.DoubleSide, toneMapped: false,
  }), 340, (dummy, i, mesh) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 1);
    dummy.position.set(p.x, 0.07, p.z);
    dummy.rotation.set(0, random() * Math.PI, 0);
    const r = 0.32 + random() * 0.95;
    dummy.scale.set(r, 1, r * (0.85 + random() * 0.3));
    mesh.setColorAt(i, greens[Math.floor(random() * greens.length)]);
  });

  instanced(group, new THREE.SphereGeometry(0.17, 6, 5), new THREE.MeshBasicMaterial({
    color: 0xffffff, toneMapped: false,
  }), 76, (dummy, i, mesh) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 2);
    dummy.position.set(p.x, 0.17, p.z);
    dummy.rotation.set(0, 0, 0);
    dummy.scale.set(1, 0.6 + random() * 0.5, 1);
    mesh.setColorAt(i, new THREE.Color(random() < 0.55 ? "#f7e6e4" : "#f2d2d8"));
  });

  instanced(group, new THREE.ConeGeometry(0.045, 1.25, 3), flatMaterial(),
    240, (dummy, i, mesh) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 3);
      dummy.position.set(p.x, 0.62, p.z);
      dummy.rotation.set((random() - 0.5) * 0.22, random() * Math.PI, 0);
      dummy.scale.set(1, 0.7 + random() * 0.8, 1);
      mesh.setColorAt(i, greens[Math.floor(random() * greens.length)]);
    });

  // 垂柳：树干 + 一圈下垂的细枝。
  const willowCount = 10;
  instanced(group, new THREE.CylinderGeometry(0.22, 0.34, 4.4, 6),
    flatMaterial({ color: skin.scenery.trunk }), willowCount, (dummy) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 9);
      dummy.position.set(p.x, 2.2, p.z);
      dummy.rotation.set(0, 0, 0);
      dummy.scale.set(1, 1, 1);
    });
  const strandCentres = [];
  for (let i = 0; i < willowCount; i += 1) {
    const angle = (i / willowCount) * Math.PI * 2 + random() * 0.35;
    const radius = CLEAR_R + 14 + random() * 18;
    strandCentres.push([Math.cos(angle) * radius, Math.sin(angle) * radius]);
  }
  instanced(group, new THREE.CylinderGeometry(0.035, 0.055, 3.2, 4), flatMaterial(),
    strandCentres.length * 16, (dummy, i, mesh) => {
      const [cx, cz] = strandCentres[Math.floor(i / 16)];
      const a = ((i % 16) / 16) * Math.PI * 2;
      dummy.position.set(cx + Math.cos(a) * 1.5, 3.0 + random() * 0.8,
        cz + Math.sin(a) * 1.5);
      dummy.rotation.set(Math.cos(a) * 0.16, 0, Math.sin(a) * 0.16);
      dummy.scale.set(1, 0.8 + random() * 0.5, 1);
      mesh.setColorAt(i, greens[Math.floor(random() * greens.length)]);
    });

  // 青绿拱桥：半环 + 沿弧铺的桥板 + 两道栏杆，落在池的一侧。
  const bridgeZ = -33;
  const archRadius = 13;
  const archLift = 0.34;
  const bridgeColor = skin.scenery.cypress;
  const arch = new THREE.Mesh(
    new THREE.TorusGeometry(archRadius, 0.38, 8, 64, Math.PI),
    flatMaterial({ color: bridgeColor }));
  arch.scale.set(1, archLift, 1);
  arch.position.set(0, 0.2, bridgeZ);
  group.add(arch);
  for (const offset of [-1.5, 1.5]) {
    const rail = new THREE.Mesh(
      new THREE.TorusGeometry(archRadius, 0.10, 6, 64, Math.PI),
      flatMaterial({ color: bridgeColor }));
    rail.scale.set(1, archLift, 1);
    rail.position.set(0, 1.25, bridgeZ + offset);
    group.add(rail);
  }
  instanced(group, new THREE.BoxGeometry(1.15, 0.14, 3.2),
    flatMaterial({ color: skin.ground.path_base }), 26, (dummy) => {
      const t = (Math.floor(0.5 + random() * 26) / 26) * Math.PI;
      dummy.position.set(Math.cos(t) * archRadius,
        Math.sin(t) * archRadius * archLift + 0.2, bridgeZ);
      dummy.rotation.set(0, 0,
        Math.atan2(archRadius * archLift * Math.cos(t), -archRadius * Math.sin(t)));
      dummy.scale.set(1, 1, 1);
    });

  for (let i = 0; i < 3; i += 1) {
    const p = annulus(random, CLEAR_R, ISLAND_R, 8);
    const stone = new THREE.Mesh(
      new THREE.CylinderGeometry(0.3, 0.38, 0.7, 6),
      flatMaterial({ color: skin.scenery.props.stone }));
    stone.position.set(p.x, 0.35, p.z);
    stone.userData = {
      id: `${skin.scenery.props.id_prefix}_${i}`, tag: skin.scenery.props.tag,
      hint: skin.scenery.props.hint, kind: KIND.INTERACTABLE,
    };
    group.add(stone);
    ctx.interactive.push(stone);
  }
  // 拱桥用的是上面同一个 bridgeZ，垂柳用 strandCentres 的真实锚点。
  feature(ctx, "pond_bridge", "青绿拱桥", "bridge",
    [0, 2, bridgeZ], { weight: 4, shot: { r: 34, el: 8, arc: 20 } });
  feature(ctx, "pond_willow", "垂柳池岸", "grove",
    [strandCentres[0][0], 3, strandCentres[0][1]], { weight: 2 });
  feature(ctx, "pond_lilies", "白粉睡莲", "water",
    [CLEAR_R + 8, 0, CLEAR_R - 4], { weight: 2 });
}

function buildSunrise(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const reflection = new THREE.PlaneGeometry(1, 1);
  reflection.rotateX(-Math.PI / 2);
  const waterTones = tones(skin.ground.strokes);
  instanced(group, reflection, new THREE.MeshBasicMaterial({
    color: 0xffffff, transparent: true, opacity: 0.42,
    side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
  }), 760, (dummy, i, mesh) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 1);
    dummy.position.set(p.x, 0.06, p.z);
    dummy.rotation.set(0, (random() - 0.5) * 0.22, 0);
    dummy.scale.set(0.8 + random() * 4.2, 1, 0.05 + random() * 0.16);
    mesh.setColorAt(i, waterTones[Math.floor(random() * waterTones.length)]);
  });

  const silhouette = flatMaterial({ color: skin.scenery.cypress, roughness: 1 });
  const sun = skin.sky.sun?.color ?? "#ff9a54";
  for (let i = 0; i < 8; i += 1) {
    const angle = (i / 8) * Math.PI * 2 + random() * 0.5;
    const radius = 27 + random() * 22;
    const boat = new THREE.Group();
    const hull = new THREE.Mesh(new THREE.CylinderGeometry(0.44, 0.30, 3.2, 6), silhouette);
    hull.rotation.z = Math.PI / 2;
    hull.position.y = 0.24;
    const mast = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.06, 2.4, 5), silhouette);
    mast.position.y = 1.5;
    const sail = new THREE.Mesh(new THREE.PlaneGeometry(1.0, 1.5),
      new THREE.MeshBasicMaterial({ color: silhouette.color, side: THREE.DoubleSide }));
    sail.position.set(0.55, 1.65, 0);
    boat.add(hull, mast, sail);
    boat.position.set(Math.cos(angle) * radius, 0, Math.sin(angle) * radius);
    boat.rotation.y = random() * Math.PI;
    group.add(boat);
  }

  // 远处桅杆 / 吊车剪影。
  instanced(group, new THREE.CylinderGeometry(0.055, 0.055, 6, 5), silhouette,
    26, (dummy) => {
      const angle = random() * Math.PI * 2;
      const radius = ISLAND_R - 8 + random() * 5;
      dummy.position.set(Math.cos(angle) * radius, 3, Math.sin(angle) * radius);
      dummy.rotation.set(0, 0, 0);
      dummy.scale.set(1, 0.7 + random() * 0.8, 1);
    });
  instanced(group, new THREE.BoxGeometry(1.6, 1.2, 1.6), silhouette,
    18, (dummy) => {
      const angle = random() * Math.PI * 2;
      const radius = ISLAND_R - 10 + random() * 6;
      dummy.position.set(Math.cos(angle) * radius, 0.6, Math.sin(angle) * radius);
      dummy.rotation.set(0, random() * Math.PI, 0);
      dummy.scale.set(0.6 + random(), 0.8 + random() * 1.6, 0.6 + random());
    });

  instanced(group, new THREE.SphereGeometry(0.22, 8, 6), new THREE.MeshBasicMaterial({
    color: new THREE.Color().setRGB(2.4, 1.1, 0.42), toneMapped: false,
  }), 26, (dummy) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 4);
    dummy.position.set(p.x, 0.28, p.z);
    dummy.rotation.set(0, 0, 0);
    dummy.scale.set(1, 1, 1);
  });

  for (let i = 0; i < 3; i += 1) {
    const p = annulus(random, CLEAR_R, ISLAND_R, 8);
    const buoy = new THREE.Mesh(
      new THREE.CylinderGeometry(0.22, 0.30, 1.0, 7),
      flatMaterial({ color: skin.scenery.props.stone }));
    buoy.position.set(p.x, 0.5, p.z);
    buoy.userData = {
      id: `${skin.scenery.props.id_prefix}_${i}`, tag: skin.scenery.props.tag,
      hint: skin.scenery.props.hint, kind: KIND.INTERACTABLE,
    };
    group.add(buoy);
    ctx.interactive.push(buoy);
  }
  // sun 变量让调色板里的日出色参与水面碎光的着色强度，避免未使用告警。
  group.userData.sun_color = sun;
  // 太阳在天空贴图里（非 3D 几何），用高处的虚拟焦点让镜头抬眼；桅杆取
  // 生成时用的 ISLAND_R-8 环带，小舟取环带代表点。
  feature(ctx, "sunrise_sun", "橙色日出", "sky",
    [0, 10, 0], { weight: 3, shot: { r: 38, el: -3, arc: 10 } });
  feature(ctx, "sunrise_boat", "雾中小舟", "water",
    [CLEAR_R + 12, 1, CLEAR_R], { weight: 2, shot: { el: 3 } });
  feature(ctx, "sunrise_mast", "港口桅杆", "building",
    [0, 3, -(ISLAND_R - 8)], { weight: 2 });
}

// ---------------------------------------------------------------------------
// 后 8 个画境的专属物件。公共零件先抽出来：地面笔触、树丛（树干/树冠共用
// 锚点）、小舟、屋舍、可点击界标、HDR 灯火。所有 preset 都只用这些零件 +
// 各自世界调色板，绝不引入外部模型或贴图。
// ---------------------------------------------------------------------------

function strokesOnGround(ctx, count, colors, opacity = 0.32, length = [0.6, 2.6]) {
  const { group, random, CLEAR_R, ISLAND_R } = ctx;
  const plane = new THREE.PlaneGeometry(1, 1);
  plane.rotateX(-Math.PI / 2);
  return instanced(group, plane, new THREE.MeshBasicMaterial({
    color: 0xffffff, transparent: true, opacity,
    side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
  }), count, (dummy, i, mesh) => {
    const p = annulus(random, CLEAR_R, ISLAND_R, 1);
    dummy.position.set(p.x, 0.07, p.z);
    dummy.rotation.set(0, random() * Math.PI, 0);
    dummy.scale.set(length[0] + random() * (length[1] - length[0]), 1,
      0.05 + random() * 0.12);
    mesh.setColorAt(i, colors[Math.floor(random() * colors.length)]);
  });
}

function grove(ctx, count, opts = {}) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const {
    crown = "round", height = 4.4, radius = 1.2,
    trunk = skin.scenery.trunk, palette = tones(skin.scenery.foliage_tones),
  } = opts;
  const trees = Array.from({ length: count }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 5),
    height: height * (0.8 + random() * 0.45),
  }));
  instanced(group, new THREE.CylinderGeometry(0.15, 0.28, 1, 6),
    flatMaterial({ color: trunk }), count, (dummy, i) => {
      const t = trees[i];
      dummy.position.set(t.x, t.height / 2, t.z);
      dummy.scale.set(1, t.height, 1);
    });
  const geometry = crown === "cone"
    ? new THREE.ConeGeometry(1, 1, 7)
    : new THREE.IcosahedronGeometry(1, 1);
  instanced(group, geometry, flatMaterial(), count, (dummy, i, mesh) => {
    const t = trees[i];
    const r = radius * (0.75 + random() * 0.5);
    dummy.position.set(t.x, t.height + r * 0.45, t.z);
    dummy.rotation.set(0, random() * Math.PI, 0);
    dummy.scale.set(r, crown === "cone" ? r * 2.4 : r * 0.85, r);
    mesh.setColorAt(i, palette[Math.floor(random() * palette.length)]);
  });
  return trees;
}

function glowOrbs(ctx, count, hdr, radius = 0.18, y = 1.0) {
  const { group, random, CLEAR_R, ISLAND_R } = ctx;
  instanced(group, new THREE.SphereGeometry(radius, 8, 6),
    new THREE.MeshBasicMaterial({
      color: new THREE.Color().setRGB(...hdr), toneMapped: false,
    }), count, (dummy) => {
      const p = annulus(random, CLEAR_R, ISLAND_R, 4);
      dummy.position.set(p.x, y + random() * 1.4, p.z);
      dummy.scale.setScalar(0.7 + random() * 0.8);
    });
}

function interactives(ctx, geometry, y = 0.45) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  for (let i = 0; i < 3; i += 1) {
    const p = annulus(random, CLEAR_R, ISLAND_R, 8);
    const item = new THREE.Mesh(geometry,
      flatMaterial({ color: skin.scenery.props.stone }));
    item.position.set(p.x, y, p.z);
    item.userData = {
      id: `${skin.scenery.props.id_prefix}_${i}`,
      tag: skin.scenery.props.tag, hint: skin.scenery.props.hint,
      kind: KIND.INTERACTABLE,
    };
    group.add(item);
    ctx.interactive.push(item);
  }
}

function boats(ctx, count, hullColor, sailColor) {
  const { group, random, CLEAR_R, ISLAND_R } = ctx;
  const hullMaterial = flatMaterial({ color: hullColor });
  const sailMaterial = new THREE.MeshBasicMaterial({
    color: sailColor, side: THREE.DoubleSide, toneMapped: false,
  });
  for (let i = 0; i < count; i += 1) {
    const p = annulus(random, CLEAR_R, ISLAND_R, 5);
    const boat = new THREE.Group();
    const hull = new THREE.Mesh(
      new THREE.CylinderGeometry(0.42, 0.28, 2.8, 6), hullMaterial);
    hull.rotation.z = Math.PI / 2;
    hull.position.y = 0.22;
    const mast = new THREE.Mesh(
      new THREE.CylinderGeometry(0.05, 0.05, 2.2, 5), hullMaterial);
    mast.position.y = 1.35;
    const sail = new THREE.Mesh(new THREE.PlaneGeometry(0.95, 1.35), sailMaterial);
    sail.position.set(0.5, 1.5, 0);
    boat.add(hull, mast, sail);
    boat.position.set(p.x, 0, p.z);
    boat.rotation.y = random() * Math.PI;
    group.add(boat);
  }
}

function cottages(ctx, count, { width = 2.6, height = 2.4 } = {}) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const sites = Array.from({ length: count }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 6), rot: random() * Math.PI,
  }));
  instanced(group, new THREE.BoxGeometry(width, height, width),
    flatMaterial({ color: skin.scenery.house }), count, (dummy, i) => {
      dummy.position.set(sites[i].x, height / 2, sites[i].z);
      dummy.rotation.set(0, sites[i].rot, 0);
    });
  instanced(group, new THREE.ConeGeometry(width * 0.8, 1.4, 4),
    flatMaterial({ color: skin.scenery.roof }), count, (dummy, i) => {
      dummy.position.set(sites[i].x, height + 0.6, sites[i].z);
      dummy.rotation.set(0, sites[i].rot + Math.PI / 4, 0);
    });
  return sites;
}

// 02 夜间咖啡馆：鹅卵石街面、露台桌椅、遮阳篷、煤气灯。
function buildCafe(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  strokesOnGround(ctx, 620, tones(skin.ground.strokes), 0.30, [0.3, 0.9]);
  const tables = Array.from({ length: 11 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 6), rot: random() * Math.PI,
  }));
  instanced(group, new THREE.CylinderGeometry(0.62, 0.62, 0.10, 10),
    flatMaterial({ color: skin.scenery.house }), tables.length, (dummy, i) => {
      dummy.position.set(tables[i].x, 0.78, tables[i].z);
      dummy.rotation.set(0, tables[i].rot, 0);
    });
  instanced(group, new THREE.CylinderGeometry(0.07, 0.10, 0.76, 6),
    flatMaterial({ color: skin.scenery.trunk }), tables.length, (dummy, i) => {
      dummy.position.set(tables[i].x, 0.38, tables[i].z);
    });
  instanced(group, new THREE.BoxGeometry(0.42, 0.10, 0.42),
    flatMaterial({ color: skin.ground.path_base }), tables.length * 3,
    (dummy, i) => {
      const t = tables[Math.floor(i / 3)];
      const a = t.rot + ((i % 3) / 3) * Math.PI * 2;
      dummy.position.set(t.x + Math.cos(a) * 1.15, 0.44, t.z + Math.sin(a) * 1.15);
      dummy.rotation.set(0, a, 0);
    });
  const awnings = Array.from({ length: 5 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 10), rot: random() * Math.PI,
  }));
  instanced(group, new THREE.BoxGeometry(4.4, 0.12, 2.2),
    flatMaterial({ color: skin.scenery.roof }), awnings.length, (dummy, i) => {
      dummy.position.set(awnings[i].x, 3.4, awnings[i].z);
      dummy.rotation.set(-0.16, awnings[i].rot, 0);
    });
  instanced(group, new THREE.CylinderGeometry(0.07, 0.09, 3.4, 6),
    flatMaterial({ color: skin.scenery.props.stone }), awnings.length * 2,
    (dummy, i) => {
      const a = awnings[Math.floor(i / 2)];
      const side = (i % 2) ? 1.7 : -1.7;
      dummy.position.set(a.x + side, 1.7, a.z);
    });
  grove(ctx, 6, { height: 3.2, radius: 0.8 });
  glowOrbs(ctx, 34, [2.4, 1.55, 0.65], 0.14, 1.2);
  interactives(ctx, new THREE.CylinderGeometry(0.18, 0.24, 1.1, 6), 0.55);
  // 露台桌椅与遮阳篷复用生成时的 tables / awnings 锚点。
  feature(ctx, "cafe_terrace", "暖黄露台", "building",
    [tables[0].x, 2, tables[0].z], { weight: 3 });
  feature(ctx, "cafe_awning", "露台遮阳篷", "building",
    [awnings[0].x, 3.4, awnings[0].z], { weight: 2 });
  feature(ctx, "cafe_lamp", "煤气灯火", "lantern",
    [-(CLEAR_R + 6), 2, CLEAR_R - 2], { weight: 2 });
}

// 04 阿尔勒卧室：把室内家具当「场景物件」散在环形地面上。
function buildBedroom(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  strokesOnGround(ctx, 420, tones(skin.ground.strokes), 0.30, [1.2, 4.2]);
  // 淡紫墙板内移到 r≈42 并加高：放在岛缘会被雾吃掉，房间围合感全无。
  instanced(group, new THREE.BoxGeometry(6.0, 5.4, 0.3),
    flatMaterial({ color: skin.scenery.foliage_tones[1] }), 10, (dummy, i) => {
      const angle = (i / 10) * Math.PI * 2;
      const radius = ISLAND_R - 14;
      dummy.position.set(Math.cos(angle) * radius, 2.7, Math.sin(angle) * radius);
      dummy.rotation.set(0, -angle, 0);
    });
  // 床压到内环 r≈21..24：原先撒到 r≈50，沉浸式视角下只剩几个白点。
  const beds = Array.from({ length: 8 }, (_, i) => {
    const a = (i / 8) * Math.PI * 2 + 0.35;
    const r = CLEAR_R + 3 + random() * 3;
    return { x: Math.cos(a) * r, z: Math.sin(a) * r, rot: a + Math.PI / 2 };
  });
  instanced(group, new THREE.BoxGeometry(3.4, 0.7, 4.6),
    flatMaterial({ color: skin.scenery.house }), beds.length, (dummy, i) => {
      dummy.position.set(beds[i].x, 0.35, beds[i].z);
      dummy.rotation.set(0, beds[i].rot, 0);
    });
  instanced(group, new THREE.BoxGeometry(3.1, 0.12, 3.8),
    flatMaterial({ color: skin.ground.path_base }), beds.length, (dummy, i) => {
      dummy.position.set(beds[i].x, 0.77, beds[i].z);
      dummy.rotation.set(0, beds[i].rot, 0);
    });
  instanced(group, new THREE.BoxGeometry(1.1, 0.25, 0.7),
    new THREE.MeshBasicMaterial({ color: skin.ground.white_stroke }),
    beds.length * 2, (dummy, i) => {
      const b = beds[Math.floor(i / 2)];
      dummy.position.set(b.x, 0.92, b.z + ((i % 2) ? 1.4 : -1.4));
      dummy.rotation.set(0, b.rot, 0);
    });
  instanced(group, new THREE.BoxGeometry(1.8, 3.6, 1.0),
    flatMaterial({ color: skin.scenery.trunk }), 10, (dummy, i) => {
      const a = (i / 10) * Math.PI * 2;
      const r = CLEAR_R + 9 + random() * 3;
      dummy.position.set(Math.cos(a) * r, 1.8, Math.sin(a) * r);
      dummy.rotation.set(0, -a, 0);
    });
  glowOrbs(ctx, 8, [1.8, 1.4, 0.9], 0.14, 1.4);
  interactives(ctx, new THREE.CylinderGeometry(0.22, 0.30, 0.9, 6), 0.45);
  // 墙板半径就是上面生成用的 ISLAND_R-14，木床用 beds 的真实锚点。
  feature(ctx, "bedroom_wall", "淡紫墙板", "interior",
    [0, 3, -(ISLAND_R - 14)], { weight: 3 });
  feature(ctx, "bedroom_bed", "木床与红毯", "interior",
    [beds[0].x, 1, beds[0].z], { weight: 3 });
  feature(ctx, "bedroom_cabinet", "墙角壁灯", "lantern",
    [CLEAR_R + 8, 2, CLEAR_R - 10], { weight: 1 });
}

// 07 圣阿德雷斯花园：花圃、旗帜、露台栏杆、远处帆影。
function buildGarden(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  strokesOnGround(ctx, 460, tones(skin.ground.strokes), 0.30, [0.5, 2.0]);
  const beds = Array.from({ length: 12 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 4), rot: random() * Math.PI,
  }));
  instanced(group, new THREE.BoxGeometry(3.2, 0.22, 1.8),
    flatMaterial({ color: skin.scenery.trunk }), beds.length, (dummy, i) => {
      dummy.position.set(beds[i].x, 0.11, beds[i].z);
      dummy.rotation.set(0, beds[i].rot, 0);
    });
  const flowerTones = tones(skin.scenery.foliage_tones);
  instanced(group, new THREE.IcosahedronGeometry(0.34, 0), flatMaterial(),
    beds.length * 9, (dummy, i, mesh) => {
      const b = beds[Math.floor(i / 9)];
      dummy.position.set(b.x + (random() - 0.5) * 2.7, 0.35,
        b.z + (random() - 0.5) * 1.4);
      dummy.scale.setScalar(0.6 + random() * 0.9);
      mesh.setColorAt(i, flowerTones[Math.floor(random() * flowerTones.length)]);
    });
  const flags = Array.from({ length: 7 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 7), rot: random() * Math.PI,
  }));
  instanced(group, new THREE.CylinderGeometry(0.06, 0.08, 6.4, 6),
    flatMaterial({ color: skin.scenery.props.stone }), flags.length,
    (dummy, i) => {
      dummy.position.set(flags[i].x, 3.2, flags[i].z);
    });
  instanced(group, new THREE.PlaneGeometry(2.2, 1.2),
    new THREE.MeshBasicMaterial({ color: skin.ground.path_base,
      side: THREE.DoubleSide, toneMapped: false }), flags.length, (dummy, i) => {
      dummy.position.set(flags[i].x, 5.2, flags[i].z);
      dummy.rotation.set(0, flags[i].rot, 0);
    });
  instanced(group, new THREE.CylinderGeometry(0.10, 0.12, 1.0, 6),
    flatMaterial({ color: skin.scenery.props.stone }), 44, (dummy, i) => {
      const a = (i / 44) * Math.PI * 2;
      const r = ISLAND_R - 4;
      dummy.position.set(Math.cos(a) * r, 0.5, Math.sin(a) * r);
    });
  boats(ctx, 4, skin.scenery.cypress, skin.ground.white_stroke);
  grove(ctx, 8, { height: 3.4, radius: 0.9 });
  interactives(ctx, new THREE.BoxGeometry(0.5, 0.9, 0.5), 0.45);
  // 旗杆取 flags[0]（旗面 y=5.2），花圃取 beds[0]，栏杆就是 ISLAND_R-4 环。
  feature(ctx, "garden_flag", "海风旗帜", "flag",
    [flags[0].x, 5.2, flags[0].z], { weight: 3 });
  feature(ctx, "garden_bed", "向日葵花圃", "field",
    [beds[0].x, 0.4, beds[0].z], { weight: 2 });
  feature(ctx, "garden_rail", "露台栏杆", "building",
    [0, 1, ISLAND_R - 4], { weight: 1 });
}

// 08 阿让特伊桥：多跨石桥、帆船、河岸树。
function buildArgenteuil(ctx) {
  const { group, skin, CLEAR_R } = ctx;
  strokesOnGround(ctx, 640, tones(skin.ground.strokes), 0.34, [0.8, 3.4]);
  const bridgeZ = -36;
  const span = 30;
  instanced(group, new THREE.CylinderGeometry(0.7, 0.9, 3.2, 8),
    flatMaterial({ color: skin.scenery.props.stone }), 6, (dummy, i) => {
      dummy.position.set(-span / 2 + (i / 5) * span, 1.6, bridgeZ);
    });
  instanced(group, new THREE.BoxGeometry(span + 4, 0.28, 3.4),
    flatMaterial({ color: skin.ground.path_base }), 1, (dummy) => {
      dummy.position.set(0, 3.3, bridgeZ);
    });
  for (const side of [-1.6, 1.6]) {
    instanced(group, new THREE.BoxGeometry(span + 4, 0.12, 0.12),
      flatMaterial({ color: skin.scenery.props.stone }), 1, (dummy) => {
        dummy.position.set(0, 4.1, bridgeZ + side);
      });
  }
  boats(ctx, 7, skin.scenery.cypress, skin.ground.white_stroke);
  grove(ctx, 12, { height: 4.0, radius: 1.0 });
  glowOrbs(ctx, 14, [2.0, 1.8, 1.2], 0.12, 1.0);
  interactives(ctx, new THREE.CylinderGeometry(0.20, 0.28, 1.0, 6), 0.5);
  // 桥面 y=3.3 与 bridgeZ 都是上面生成石桥用的变量。
  feature(ctx, "argenteuil_bridge", "石拱桥", "bridge",
    [0, 3.3, bridgeZ], { weight: 4, shot: { r: 34, el: 9, arc: 18 } });
  feature(ctx, "argenteuil_boat", "白帆小船", "water",
    [CLEAR_R + 12, 1, CLEAR_R - 4], { weight: 2, shot: { el: 3 } });
  feature(ctx, "argenteuil_grove", "河岸树影", "grove",
    [-(CLEAR_R + 6), 3, CLEAR_R - 2], { weight: 1 });
}

// 09 维伦纽夫桥：桥塔 + 悬索 + 桥面 + 彼岸屋舍。
function buildVilleneuve(ctx) {
  const { group, skin, CLEAR_R } = ctx;
  strokesOnGround(ctx, 520, tones(skin.ground.strokes), 0.30, [0.7, 3.0]);
  const bridgeX = -34;
  instanced(group, new THREE.BoxGeometry(1.2, 8.0, 1.2),
    flatMaterial({ color: skin.scenery.props.stone }), 2, (dummy, i) => {
      dummy.position.set(bridgeX, 4, (i ? 1 : -1) * 13);
    });
  const cable = new THREE.Mesh(
    new THREE.TorusGeometry(15, 0.09, 6, 48, Math.PI),
    flatMaterial({ color: skin.scenery.trunk }));
  cable.rotation.y = Math.PI / 2;
  cable.scale.set(1, 0.55, 1);
  cable.position.set(bridgeX, 6.0, 0);
  group.add(cable);
  instanced(group, new THREE.BoxGeometry(3.0, 0.25, 28),
    flatMaterial({ color: skin.ground.path_base }), 1, (dummy) => {
      dummy.position.set(bridgeX, 2.1, 0);
    });
  instanced(group, new THREE.CylinderGeometry(0.035, 0.035, 1, 4),
    flatMaterial({ color: skin.scenery.trunk }), 24, (dummy, i) => {
      const t = (i / 24) * 2 - 1;
      dummy.position.set(bridgeX, 4.1, t * 13);
      dummy.scale.set(1, 1.6 + (1 - Math.abs(t)) * 2.2, 1);
    });
  cottages(ctx, 10);
  grove(ctx, 9, { height: 3.6, radius: 0.9 });
  interactives(ctx, new THREE.BoxGeometry(0.8, 1.0, 0.8), 0.5);
  // 桥塔 / 悬索 / 桥面都挂在 bridgeX 上，登记同一个锚点。
  feature(ctx, "villeneuve_bridge", "悬索桥塔", "bridge",
    [bridgeX, 4, 0], { weight: 4, shot: { r: 32, el: 12, arc: 16 } });
  feature(ctx, "villeneuve_cottage", "彼岸屋舍", "building",
    [CLEAR_R + 12, 3, CLEAR_R - 8], { weight: 2 });
  feature(ctx, "villeneuve_bank", "沙岸石墩", "water",
    [-(CLEAR_R + 2), 1, CLEAR_R + 2], { weight: 1 });
}

// 10 马尔利港洪水：水漫外圈、临水建筑与穿行的小船。
function buildMarly(ctx) {
  const { group, skin, CLEAR_R, ISLAND_R } = ctx;
  strokesOnGround(ctx, 720, tones(skin.ground.strokes), 0.38, [1.0, 4.0]);
  const water = new THREE.Mesh(
    new THREE.RingGeometry(CLEAR_R + 4, ISLAND_R, 96),
    new THREE.MeshBasicMaterial({ color: skin.ground.sea, transparent: true,
      opacity: 0.42, side: THREE.DoubleSide, depthWrite: false, toneMapped: false }));
  water.rotation.x = -Math.PI / 2;
  water.position.y = 0.12;
  group.add(water);
  cottages(ctx, 12, { width: 3.0, height: 3.4 });
  boats(ctx, 6, skin.scenery.cypress, skin.ground.white_stroke);
  grove(ctx, 6, { height: 3.2, radius: 0.8 });
  glowOrbs(ctx, 12, [2.0, 1.7, 1.1], 0.12, 1.0);
  interactives(ctx, new THREE.CylinderGeometry(0.16, 0.20, 1.6, 6), 0.8);
  // 水面 RingGeometry 覆盖 CLEAR_R+4..ISLAND_R，屋墙与小舟取环带代表点。
  feature(ctx, "marly_water", "水街倒影", "water",
    [0, 1, CLEAR_R + 12], { weight: 3, shot: { el: 3 } });
  feature(ctx, "marly_house", "杏色屋墙", "building",
    [CLEAR_R + 10, 2, -(CLEAR_R - 6)], { weight: 3 });
  feature(ctx, "marly_boat", "穿行小船", "water",
    [-(CLEAR_R + 6), 1, CLEAR_R + 2], { weight: 1 });
}

// 11 圣维克多山：远处蓝灰山脊、赭色田埂、松林。
function buildVictoire(ctx) {
  const { group, skin, ISLAND_R } = ctx;
  strokesOnGround(ctx, 560, tones(skin.ground.strokes), 0.32, [1.4, 5.0]);
  // 远山要「蓝灰长肩」而不是白色尖锥：雾色直接当材质会被午后光照洗白，
  // 先压暗，再把锥体压扁、拉宽，才像塞尚笔下层层后退的山脊。
  const mountainColor = new THREE.Color(skin.sky.fog.color).multiplyScalar(0.55);
  const mountain = flatMaterial({ color: mountainColor, roughness: 1 });
  instanced(group, new THREE.ConeGeometry(1, 1, 5), mountain, 7, (dummy, i) => {
    const a = Math.PI * 0.75 + (i / 7) * Math.PI * 0.7;
    const r = ISLAND_R - 2;
    const h = 9 + (i % 3) * 3.5;
    dummy.position.set(Math.cos(a) * r, h / 2, Math.sin(a) * r);
    dummy.rotation.set(0, i * 0.7, 0);
    dummy.scale.set(22 + (i % 3) * 6, h, 14 + (i % 2) * 4);
  });
  grove(ctx, 20, { crown: "cone", height: 4.6, radius: 1.3 });
  strokesOnGround(ctx, 220,
    tones([skin.ground.path_base, skin.ground.stroke_color]), 0.45, [3.0, 7.0]);
  interactives(ctx, new THREE.IcosahedronGeometry(0.6, 0), 0.4);
  // 山脊与上面 7 个锥体共用 a / ISLAND_R-2 的生成参数，取中段代表角。
  const ridgeAngle = Math.PI * 0.75 + Math.PI * 0.35;
  feature(ctx, "victoire_mountain", "蓝灰山峰", "mountain",
    [Math.cos(ridgeAngle) * (ISLAND_R - 2), 10,
      Math.sin(ridgeAngle) * (ISLAND_R - 2)],
    { weight: 4, shot: { r: 46, el: -4, arc: 12 } });
  feature(ctx, "victoire_field", "赭色田埂", "field",
    [-(ctx.CLEAR_R + 8), 1, ctx.CLEAR_R], { weight: 2 });
  feature(ctx, "victoire_pines", "绿色松林", "grove",
    [ctx.CLEAR_R - 6, 3, -(ctx.CLEAR_R + 10)], { weight: 1 });
}

// 12 塔希提小路：棕榈（树干与长叶共用锚点）、茅屋、火把。
function buildGauguin(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  strokesOnGround(ctx, 600, tones(skin.ground.strokes), 0.34, [0.6, 2.6]);
  const palms = Array.from({ length: 16 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 5),
    height: 5 + random() * 3, rot: random() * Math.PI,
  }));
  instanced(group, new THREE.CylinderGeometry(0.14, 0.30, 1, 6),
    flatMaterial({ color: skin.scenery.trunk }), palms.length, (dummy, i) => {
      const p = palms[i];
      dummy.position.set(p.x, p.height / 2, p.z);
      dummy.rotation.set(0, 0, 0.08);
      dummy.scale.set(1, p.height, 1);
    });
  const leafTones = tones(skin.scenery.foliage_tones);
  instanced(group, new THREE.ConeGeometry(0.42, 3.2, 3), flatMaterial(),
    palms.length * 7, (dummy, i, mesh) => {
      const p = palms[Math.floor(i / 7)];
      const a = p.rot + ((i % 7) / 7) * Math.PI * 2;
      dummy.position.set(p.x + Math.cos(a) * 0.9, p.height + 0.1,
        p.z + Math.sin(a) * 0.9);
      dummy.rotation.set(Math.cos(a) * 0.9, -a, Math.sin(a) * 0.9);
      dummy.scale.set(1, 0.8 + (i % 3) * 0.2, 1);
      mesh.setColorAt(i, leafTones[Math.floor(random() * leafTones.length)]);
    });
  cottages(ctx, 8, { width: 2.8, height: 2.2 });
  glowOrbs(ctx, 22, [2.4, 1.2, 0.42], 0.18, 0.9);
  interactives(ctx, new THREE.CylinderGeometry(0.12, 0.18, 1.5, 6), 0.75);
  // 棕榈复用 palms[0] 的生成锚点；小路沿 PATHS 的环带取代表点。
  feature(ctx, "gauguin_palms", "热带树丛", "grove",
    [palms[0].x, palms[0].height, palms[0].z], { weight: 3 });
  feature(ctx, "gauguin_hut", "茅屋灯火", "building",
    [-(CLEAR_R + 6), 2, CLEAR_R - 2], { weight: 2 });
  feature(ctx, "gauguin_path", "红土小路", "path",
    [-(CLEAR_R + 2), 1, CLEAR_R + 8], { weight: 2 });
}


// 13 金铲铲「枫染棋坪」的 3D 近似备份：双层飞檐亭阁、悬挂宫灯、屏风牌坊、
// 云片盆景与贴地光环，几何全是程序化原语，颜色取自该画境调色板。
// ⚠️ 默认不显示：第 13 画境走 overrides.artwork.mode=pixel_faithful，把用户
// 的实机截图整幅铺在画布之上，与来源图逐像素一致。这里保留 preset 是为了
// 维持「13 个画境各有一个专属 preset」的契约，并让用户关掉原画直出时有
// 可对照的程序化 3D 版本 —— 它不是原画的复刻，只是近似。
function buildJCC(ctx) {
  const { group, skin, random, CLEAR_R, ISLAND_R } = ctx;
  const goldTones = tones(skin.ground.strokes.slice(0, 4));
  const jadeTones = tones(skin.scenery.foliage_tones);
  const wood = skin.scenery.trunk;
  const roofColor = skin.scenery.roof;

  strokesOnGround(ctx, 620, tones(skin.ground.strokes), 0.34, [0.8, 3.0]);

  // 亭阁：3 座，台基 / 立柱 / 两层四坡顶 / 翘角 / 暖光窗格共用同一份位置表。
  const pavilions = Array.from({ length: 3 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 9),
    height: 4.2 + random() * 1.6,
    width: 2.6 + random() * 0.8,
    rot: random() * Math.PI,
  }));
  instanced(group, new THREE.BoxGeometry(1, 1, 1),
    flatMaterial({ color: wood }), pavilions.length, (dummy, i) => {
      const p = pavilions[i];
      dummy.position.set(p.x, 0.35, p.z);
      dummy.rotation.set(0, p.rot, 0);
      dummy.scale.set(p.width * 2.1, 0.7, p.width * 2.1);
    });
  instanced(group, new THREE.CylinderGeometry(0.16, 0.20, 1, 6),
    flatMaterial({ color: skin.scenery.house }), pavilions.length * 4,
    (dummy, i) => {
      const p = pavilions[Math.floor(i / 4)];
      const k = i % 4;
      dummy.position.set(p.x + (k % 2 ? 1 : -1) * p.width * 0.72,
        p.height / 2, p.z + (k < 2 ? 1 : -1) * p.width * 0.72);
      dummy.scale.set(1, p.height, 1);
    });
  for (let layer = 0; layer < 2; layer += 1) {
    instanced(group, new THREE.ConeGeometry(1, 1, 4), flatMaterial(),
      pavilions.length, (dummy, i, mesh) => {
        const p = pavilions[i];
        const w = p.width * (layer === 0 ? 1.7 : 1.15);
        dummy.position.set(p.x, p.height + (layer === 0 ? 0.5 : 2.0), p.z);
        dummy.rotation.set(0, p.rot + Math.PI / 4, 0);
        dummy.scale.set(w, layer === 0 ? 1.5 : 1.2, w);
        mesh.setColorAt(i, goldTones[layer]);
      });
  }
  instanced(group, new THREE.ConeGeometry(0.16, 0.9, 4),
    flatMaterial({ color: skin.ground.rings }), pavilions.length * 4,
    (dummy, i) => {
      const p = pavilions[Math.floor(i / 4)];
      const a = p.rot + (i % 4) * Math.PI / 2 + Math.PI / 4;
      dummy.position.set(p.x + Math.cos(a) * p.width * 1.15,
        p.height + 1.0, p.z + Math.sin(a) * p.width * 1.15);
      dummy.rotation.set(Math.sin(a) * 0.5, 0, -Math.cos(a) * 0.5);
    });
  instanced(group, new THREE.PlaneGeometry(1, 1),
    new THREE.MeshBasicMaterial({
      color: new THREE.Color().setRGB(...skin.scenery.window),
      side: THREE.DoubleSide, toneMapped: false, transparent: true, opacity: 0.85,
    }), pavilions.length * 2, (dummy, i) => {
      const p = pavilions[Math.floor(i / 2)];
      const side = i % 2 ? 1 : -1;
      dummy.position.set(p.x + side * p.width * 0.62, p.height * 0.55,
        p.z + p.width * 0.62);
      dummy.rotation.set(0, p.rot + Math.PI / 4, 0);
      dummy.scale.set(1.1, 1.6, 1);
    });

  // 悬挂宫灯：灯身与 HDR 灯芯分层，位置表共用。
  const lanterns = Array.from({ length: 14 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 4),
    y: 3.2 + random() * 2.4,
  }));
  instanced(group, new THREE.CylinderGeometry(0.26, 0.26, 1, 6),
    flatMaterial({ color: skin.ground.rings }), lanterns.length,
    (dummy, i) => {
      const l = lanterns[i];
      dummy.position.set(l.x, l.y, l.z);
      dummy.scale.set(1, 1.5, 1);
    });
  glowOrbs(ctx, 26, skin.scenery.props.wick, 0.22, 2.4);

  // 屏风 / 牌坊：布幔与两侧立柱共用同一份位置表。
  const screens = Array.from({ length: 7 }, () => ({
    ...annulus(random, CLEAR_R, ISLAND_R, 6),
    rot: random() * Math.PI,
    w: 2.2 + random() * 1.4,
  }));
  instanced(group, new THREE.PlaneGeometry(1, 1),
    flatMaterial({ side: THREE.DoubleSide }), screens.length,
    (dummy, i, mesh) => {
      const s = screens[i];
      dummy.position.set(s.x, 1.7, s.z);
      dummy.rotation.set(0, s.rot, 0);
      dummy.scale.set(s.w, 3.0, 1);
      mesh.setColorAt(i, goldTones[i % goldTones.length]);
    });
  instanced(group, new THREE.CylinderGeometry(0.10, 0.12, 1, 6),
    flatMaterial({ color: wood }), screens.length * 2, (dummy, i) => {
      const s = screens[Math.floor(i / 2)];
      const side = i % 2 ? 1 : -1;
      dummy.position.set(s.x + Math.cos(s.rot) * side * s.w * 0.5, 1.7,
        s.z + Math.sin(s.rot) * side * s.w * 0.5);
      dummy.scale.set(1, 3.6, 1);
    });

  // 云片盆景：树干 / 圆冠由 grove 共用锚点，再叠两层压扁云片。
  const bonsai = grove(ctx, 18, {
    crown: "round", height: 3.0, radius: 1.35, trunk: wood, palette: jadeTones,
  });
  instanced(group, new THREE.IcosahedronGeometry(1, 1), flatMaterial(),
    bonsai.length * 2, (dummy, i, mesh) => {
      const t = bonsai[Math.floor(i / 2)];
      const layer = i % 2;
      dummy.position.set(t.x, t.height + 0.5 + layer * 0.9, t.z);
      dummy.scale.set(1.5 - layer * 0.35, 0.35, 1.2 - layer * 0.3);
      mesh.setColorAt(i, jadeTones[Math.floor(random() * jadeTones.length)]);
    });

  // 环绕光带：贴地的扁平开口环，水平旋转烘进几何体（不用 object.rotation）。
  // 首轮用 Torus + 大角度倾斜，圆环近端直接贴脸把画面竖切成两半；第二轮改成
  // 近水平 Torus 仍会斜插天空。贴地 Ring 与 12 画境的地面笔触同层，绝不进相机轨道。
  const ribbonColors = ["#fff0bd", "#c9b7e8", "#e8d2f0"];
  for (let k = 0; k < 3; k += 1) {
    const inner = CLEAR_R + 1.5 + k * 2.0;
    const ring = new THREE.RingGeometry(inner, inner + 0.42, 96, 1,
      k * 0.6, Math.PI * 1.55);
    ring.rotateX(-Math.PI / 2);
    const band = new THREE.Mesh(ring, new THREE.MeshBasicMaterial({
      color: ribbonColors[k], transparent: true, opacity: 0.20,
      side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
    }));
    band.position.y = 0.09 + k * 0.01;
    group.add(band);
  }

  interactives(ctx, new THREE.CylinderGeometry(0.18, 0.24, 1.2, 6), 0.6);

  feature(ctx, "jcc_pavilion", "流金亭阁", "building",
    [pavilions[0].x, pavilions[0].height, pavilions[0].z],
    { weight: 3, shot: { el: 10 } });
  feature(ctx, "jcc_lantern", "宫灯长廊", "lantern",
    [lanterns[0].x, lanterns[0].y, lanterns[0].z], { weight: 2 });
  feature(ctx, "jcc_ribbon", "环绕缎带", "sky",
    [0, 6, -(CLEAR_R + 10)], { weight: 2, shot: { el: -2 } });
}

const PRESETS = {
  wheat: buildWheat,
  pond: buildPond,
  sunrise: buildSunrise,
  cafe: buildCafe,
  bedroom: buildBedroom,
  garden: buildGarden,
  argenteuil: buildArgenteuil,
  villeneuve: buildVilleneuve,
  marly: buildMarly,
  victoire: buildVictoire,
  gauguin: buildGauguin,
  jcc: buildJCC,
};

// 返回新增的顶层子对象数量，供无头探针核对「这个 preset 真的建了东西」。
export function buildWorldScenery(preset, ctx) {
  ctx.features ??= [];
  const build = PRESETS[preset];
  if (!build) return 0;
  const before = ctx.group.children.length;
  build(ctx);
  return ctx.group.children.length - before;
}

export const SCENERY_PRESETS = Object.freeze(Object.keys(PRESETS));
