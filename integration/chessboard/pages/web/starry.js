// 沉浸式宣发页（starry.html）：与数据验证页 app.js 完全独立，互不影响。
//
// 分工：index.html / app.js = 三端数据一致性预览（LOD / 变体 / 清单校验）；
//       本页 = 2.png 那种「走进星夜」的沉浸式场景，构图、天空、地面、氛围优先。
//
// 资产仍然同源，不另起一套：
//   - 天空 = visual-pack.json 里 sha 锁定的 assets/sky_panorama.jpg（与 Unity Volume
//     的 skybox / Blender World 同一张图），按天穹球贴，不做几何测量。
//   - 棋盘 = **画进地形贴图的一块区域**，不是盖在地上的独立 3D 台面。区域轮廓
//     取 build/board.zbrd 里 HD 档 core_slab 顶面顶点的 XZ 凸包（冻结契约唯一
//     真源），再按场地线的方式描边 / 画中圈徽记；笔触地形继续从区域里穿过，
//     所以它是「同一片地面被划出一块」，与现网棋盘（4.png：木地板 + 白线 +
//     中圈徽记，与周围地面共面）同构。本页不渲染任何 ZBRD 台面 / 包边 /
//     座席实体，也不渲染 sky@* 程序化天穹球（让位给全景天空盒）。
//   - 场景物件 = buildSceneProps() 生成的冷蓝石灯 / 矮石座，沿小路散布，与树丛、
//     屋舍同色系，对应 4.png 里场地边上的篮球架 / 长椅 / 球车。用户实测围在棋盘
//     四周的 6 个金色混元单体跟星夜不匹配，所以默认**不陈列**它们
//     （USE_EXTERNAL_PROPS = false）；loadExternalMeshes() 及其 sha / 锚点 / 贴图
//     校验代码保留，改 true 即可切回来验收。验证页 app.js / Unity / Blender 仍照常
//     消费 assets/external_meshes/*.zbrd + *.png，三端同源不受影响。
//   - 地面 = 按 3.png 程序化生成的圆形星夜岛（笔触贴图 / 蜿蜒小路 / 柏树 / 树丛 /
//     屋舍 / 白色笔触），中央 CLEAR_R 半径内不放物件，留给画出来的棋盘区域。

//   - 12 套视觉包（web/worlds.json）：同一个渲染器按 ?world=<id> 切天空 /
//     地面笔触 / 场地线 / 场景物件 / 灯光。几何真源仍是同一份 ZBRD，视觉包
//     只定调，不改轮廓。starry-night 继续用 sha 锁定的全景图，其余世界用
//     world-sky.mjs 按调色板程序化画 2:1 天穹，离线可复现、不依赖临时 URL。

import * as THREE from "three";
import { parseZBRD, filterByLod, KIND } from "./zbrd.js";
import {
  EXTERNAL_MANIFEST_SCHEMA, applyTransform, groupPlacements, verifySha256,
} from "./external_mesh.js";
import {
  resolveWorldPack, selectedWorldId, applyWorldHud, WORLD_PACK_SCHEMA,
} from "./world-pack.js";
import { makeSkyPanorama } from "./world-sky.js";
import { buildWorldScenery } from "./world-scenery.js";
import { createCeremony } from "./board-ceremony.js";

const BUILD = "../build/";
const $ = (id) => document.getElementById(id);
const DEG = Math.PI / 180;
const ISLAND_R = 56;
const CLEAR_R = 18;

// 沉浸式页默认只摆「场景物件」（石灯 / 矮石座），不再把混元单体当成围在
// 棋盘四周的金色装饰物 —— 与 4.png 里场地边上的篮球架 / 长椅同类，跟夜色同调。
// 想在同一页验收那 6 个混元单体时把它改成 true 即可，加载/校验代码都保留。
const USE_EXTERNAL_PROPS = false;

// 小路走向同时被地面 ribbon 与沿路石灯复用，避免两处各写一套坐标。
const PATHS = [
  { width: 2.7, points: [[0, -50], [3, -40], [14, -32], [24, -22], [26, -8],
    [24, 8], [16, 22], [5, 34], [0, 50]] },
  { width: 2.3, points: [[-48, 24], [-34, 26], [-22, 25], [-10, 28],
    [6, 27], [22, 25], [38, 26], [48, 24]] },
  { width: 2.0, points: [[-46, -10], [-38, -22], [-30, -31], [-20, -37],
    [-8, -38], [4, -36], [14, -32]] },
];

const state = {
  spec: null, manifest: null, visual: null, ext: null,
  worlds: null, pack: null, skin: null,
  interactive: [], hints: new Map(),
  features: [],              // 场景生成时登记的地标，入场分镜从这里派生
  external: { applied: 0, total: 0, textured: 0, error: "" },
  postAvailable: false,
};

// 棋盘演出：事件契约在 ../ceremony.json，DOM 表现由 board-ceremony.mjs 负责。
// entrance 在 3D 侧是一段「环绕画境特色」的入场镜头（临时接管 cam，结束即复位）；
// 其余事件只做短暂氛围脉冲，不写回 cam，避免干扰环视与像素回归。
let ceremonyDoc = null;
let ceremony = null;
let ceremonyPulse = null;
let entranceShot = null;   // 入场环绕镜头；null = 没有正在播放的入场
let entranceFx = null;     // 方格亮起 / 扩散光圈 / 灯光渐亮 / bloom 的入场光效
const GROUND_PLANE = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(66, innerWidth / innerHeight, 0.1, 1400);
const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
$("stage").appendChild(renderer.domElement);

const islandGroup = new THREE.Group();
const boardGroup = new THREE.Group();
const scenePropsGroup = new THREE.Group();
const fxGroup = new THREE.Group();
// 棋盘只是画在地面贴图上，地面与场景物件底部同在 y=0；不再有共面的实体台面，
// 也就没有俯瞰时的 z-fighting 摩尔纹。boardGroup 仅在 USE_EXTERNAL_PROPS 时
// 才装那 6 个可选的混元单体，默认场景走 scenePropsGroup。
scene.add(islandGroup, boardGroup, scenePropsGroup, fxGroup);

// 轨道相机：target 可以高于相机（elevation < 0），从而得到 2.png 那种
// 「天空占 2/3、地面占 1/3」的仰视构图；elevation 60°+ 就是 3.png 的俯瞰。
const PRESETS = {
  immersive: { targetY: 6.0, radius: 32, elevation: -5, azimuth: 0, panX: 0, panZ: 0, fov: 66 },
  overview: { targetY: 0.0, radius: 82, elevation: 62, azimuth: 0, panX: 0, panZ: 0, fov: 52 },
};

// 竖屏时 66° 的垂直 FOV 只换来约 33° 水平视野，桌面构图会被裁成一条。
// 按视口放宽 FOV、拉远相机，同时把拖拽 sweep 调小：窄屏上同样 40px 手指
// 位移对应更大的视口比例，直接沿用桌面 sweep 会转得过猛。
function viewportTuning() {
  const w = Math.max(1, innerWidth);
  const h = Math.max(1, innerHeight);
  const compact = Math.min(w, h) < 760;
  if (!compact) return { key: "desktop", fov: 1, radius: 1, yaw: 1, tap: 5 };
  return h / w > 1.15
    ? { key: "portrait", fov: 1.30, radius: 1.34, yaw: 0.70, tap: 12 }
    : { key: "landscape", fov: 1.06, radius: 1.10, yaw: 0.85, tap: 10 };
}

function tunedPreset(mode) {
  const base = PRESETS[mode];
  const tuning = viewportTuning();
  return { ...base, fov: base.fov * tuning.fov, radius: base.radius * tuning.radius };
}

const cam = { ...tunedPreset("immersive") };
let viewMode = "immersive";
let lastTuneKey = viewportTuning().key;
let tween = null;
let tour = false;
let dragging = null;
const raycaster = new THREE.Raycaster();
const pointer = new THREE.Vector2();

let composer = null;
let bloomPass = null;
let skyMesh = null;
let dust = null;

function updateCamera(time = 0) {
  const el = cam.elevation * DEG;
  const target = new THREE.Vector3(cam.panX, cam.targetY, cam.panZ);
  const position = new THREE.Vector3(
    target.x + cam.radius * Math.cos(el) * Math.sin(cam.azimuth),
    target.y + cam.radius * Math.sin(el),
    target.z + cam.radius * Math.cos(el) * Math.cos(cam.azimuth));
  if (cam.elevation < 20) position.y += Math.sin(time * 0.0007) * 0.08;
  camera.position.copy(position);
  camera.fov = cam.fov;
  camera.updateProjectionMatrix();
  camera.lookAt(target);
}
updateCamera();

function fail(message) {
  $("loading").classList.add("done");
  const box = $("error");
  box.hidden = false;
  box.textContent = message;
}

async function fetchJSON(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path} -> HTTP ${response.status}`);
  return response.json();
}

async function fetchBytes(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path} -> HTTP ${response.status}`);
  return response.arrayBuffer();
}

// ZBRD / visual-pack 里的颜色是 sRGB 设计值，three.js 在线性空间工作。
function srgb(rgba) {
  return new THREE.Color().setRGB(rgba[0], rgba[1], rgba[2], THREE.SRGBColorSpace);
}

// worlds.json 用 #rrggbb / [r,g,b] 写调色板；转换成 srgb() 需要的 0..1 四元组。
function hexRgba(hex) {
  const value = parseInt(hex.slice(1), 16);
  return [((value >> 16) & 255) / 255, ((value >> 8) & 255) / 255,
    (value & 255) / 255, 1];
}

function rgbHex(rgb) {
  return "#" + rgb.map((v) => Math.round(v).toString(16).padStart(2, "0")).join("");
}

// 把视觉包并进 visual-pack 的运行时副本：灯光 / 雾 / bloom / 曝光从这个
// 对象里读，sha 锁定的全景图字段仍保留，保证星夜页同源语义不变。
function worldVisual(base, pack) {
  return {
    ...base,
    palette: {
      primary: hexRgba(pack.ground.gradient[1]),
      accent: hexRgba(rgbHex(pack.board.line)),
      glow: hexRgba(pack.lighting.lamps),
    },
    skybox: {
      ...base.skybox,
      panorama_image: pack.sky.panorama_image ?? base.skybox.panorama_image,
      panorama_sha256: pack.sky.panorama_sha256 ?? base.skybox.panorama_sha256,
      rotation_deg: pack.sky.rotation_deg ?? 0,
      fog: {
        ...base.skybox.fog,
        color: hexRgba(pack.sky.fog.color), density: pack.sky.fog.density,
      },
    },
    post_processing: {
      ...base.post_processing,
      exposure_value: pack.sky.exposure,
      bloom: {
        ...base.post_processing.bloom,
        threshold: pack.sky.bloom.threshold,
        intensity: pack.sky.bloom.strength,
        diffusion: pack.sky.bloom.radius,
      },
    },
    lighting: {
      ...base.lighting,
      directional_color: hexRgba(pack.lighting.directional_color),
      directional_intensity: pack.lighting.directional_intensity,
      ambient_color: hexRgba(pack.lighting.ambient_color),
      ambient_intensity: pack.lighting.ambient_intensity,
    },
  };
}

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// 逐字节校验后由 Blob URL 解码；glb 内嵌贴图让 GLTFLoader 的 ImageBitmapLoader
// 在无头环境挂起，所以这里与验证页一样走 TextureLoader。
async function loadShaTexture(rel, expected) {
  const bytes = await fetchBytes("../" + rel);
  if (expected && !(await verifySha256(bytes, expected))) {
    throw new Error(`${rel} sha256 校验失败`);
  }
  const type = rel.endsWith(".png") ? "image/png" : "image/jpeg";
  const url = URL.createObjectURL(new Blob([bytes], { type }));
  try {
    return await new THREE.TextureLoader().loadAsync(url);
  } finally {
    URL.revokeObjectURL(url);
  }
}

function makeGeometry(part) {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(part.positions, 3));
  geometry.setAttribute("normal", new THREE.BufferAttribute(part.normals, 3));
  geometry.setAttribute("uv", new THREE.BufferAttribute(part.uvs, 2));
  geometry.setIndex(new THREE.BufferAttribute(part.indices, 1));
  geometry.computeBoundingSphere();
  return geometry;
}

// ---------------------------------------------------------------- 天空盒
// 全景图是 2:1 的「全景风格图」，不是严格等距柱状投影；把 v 压到地平线以上
// 约 45° 的天空带里（factor≈4），新月才会落在相机看得到的高度而不是天顶。
// 地平线以下交给「海面」圆盘遮住，避免全景图底部被拉成一大片粉色。
function buildSky(visual) {
  const texture = state.skyTexture;
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());

  const geometry = new THREE.SphereGeometry(420, 64, 48);
  const uv = geometry.attributes.uv;
  for (let i = 0; i < uv.count; i += 1) {
    uv.setY(i, THREE.MathUtils.clamp(0.02 + (uv.getY(i) - 0.5) * 3.9, 0.02, 1));
  }
  uv.needsUpdate = true;
  const material = new THREE.MeshBasicMaterial({
    map: texture, side: THREE.BackSide, depthWrite: false, fog: false, toneMapped: false,
  });
  skyMesh = new THREE.Mesh(geometry, material);
  skyMesh.renderOrder = -1000;
  skyMesh.frustumCulled = false;
  skyMesh.rotation.y = (visual.skybox.rotation_deg ?? 0) * DEG;
  skyMesh.name = "sky_panorama";
  scene.add(skyMesh);
}

// ---------------------------------------------------------------- 星夜岛
// ------------------------------------------------- 棋盘区域：画进地形，不盖上去
// CircleGeometry 的 uv=(x/2R+.5, y/2R+.5)，绕 X 转 -90° 后世界 z=-y；
// CanvasTexture 默认 flipY，所以画布像素 = centre + (x,z) * size/(2R)。
// 这个映射让 ZBRD 的世界米坐标直接落到地面贴图上，不用另写一套棋盘几何公式。
function boardProjection(size) {
  const centre = size / 2;
  const scale = size / (2 * ISLAND_R);
  return {
    centre, scale,
    toPx: (x, z) => [centre + x * scale, centre + z * scale],
  };
}

function traceOutline(ctx, board, projection, inset = 1) {
  ctx.beginPath();
  board.outline.forEach(([x, z], i) => {
    const [px, py] = projection.toPx(x * inset, z * inset);
    if (i === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  });
  ctx.closePath();
}

// 场地线要像被画笔扫过，不能像 CAD 描边：沿每条边细分并加一点点抖动。
function handStroke(ctx, points, projection, jitter) {
  ctx.beginPath();
  for (let i = 0; i < points.length; i += 1) {
    const [x0, z0] = points[i];
    const [x1, z1] = points[(i + 1) % points.length];
    for (let k = 0; k < 6; k += 1) {
      const t = k / 6;
      const jx = (Math.sin(i * 17.3 + k * 4.1) + Math.cos(i * 7.7 + k * 2.3)) * jitter;
      const jz = (Math.cos(i * 11.9 + k * 3.7) - Math.sin(i * 5.1 + k * 1.9)) * jitter;
      const [px, py] = projection.toPx(x0 + (x1 - x0) * t + jx,
        z0 + (z1 - z0) * t + jz);
      if (i === 0 && k === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
  }
  ctx.closePath();
  ctx.stroke();
}

// 底色只在笔触之前淡淡铺一层，并用径向渐变把边缘羽化到 0：
// 区域是「同一片地面略亮的一块」，不是一块纯色填充。
function paintBoardBase(ctx, board, projection) {
  const { centre, scale } = projection;
  const reach = Math.max(board.half[0], board.half[1]) * scale * 1.35;
  ctx.save();
  traceOutline(ctx, board, projection, 1.04);
  ctx.clip();
  const glow = ctx.createRadialGradient(centre, centre, 0, centre, centre, reach);
  const stops = state.skin.board.base;
  glow.addColorStop(0, stops[0]);
  glow.addColorStop(0.55, stops[1]);
  glow.addColorStop(1, stops[2]);
  ctx.fillStyle = glow;
  ctx.fillRect(centre - reach, centre - reach, reach * 2, reach * 2);
  ctx.restore();
}

// 2026-11-06 用户纠正：棋盘只冻结「最外面一个方格 + 左下/右上两个棋手占位」。
// 概念草图里的中线、两个半圆、中圈同心圆、外侧小圆都不是必须元素，默认一条都不画；
// 只有当某张概念图在 worlds.json 的 overrides.board.marks 里显式声明时才绘制，
// 颜色同样取自该视觉包 —— 这就是「其它根据概念图来输出」。
function paintBoardLines(ctx, board, projection) {
  const { scale, toPx } = projection;
  const [hx, hz] = board.half;
  ctx.save();
  ctx.lineJoin = "round";
  ctx.lineCap = "round";

  const boardSkin = state.skin.board;
  const marks = boardSkin.marks ?? [];

  // 1) 唯一必须元素：最外面的方格，轮廓直接取 ZBRD core_slab 的凸包。
  const rim = board.outline.map(([x, z]) => [x * 0.94, z * 0.94]);
  ctx.strokeStyle = boardSkin.rim_halo;
  ctx.lineWidth = 9;
  handStroke(ctx, rim, projection, 0.05);
  ctx.strokeStyle = boardSkin.rim;
  ctx.lineWidth = 1.7;
  handStroke(ctx, rim, projection, 0.05);

  // 2) 两个棋手占位：左下 / 右上对角，位置与半径全部来自 ZBRD 的 anchor_*。
  //    这里只标「占位区」，桌椅等具体造型由概念图输出，不进冻结几何。
  const [cx, cy] = toPx(0, 0);
  for (const [ax, az, radius] of board.anchors) {
    const [px, py] = toPx(ax, az);
    const r = Math.max(6, radius * scale);
    ctx.fillStyle = boardSkin.anchor;
    ctx.beginPath();
    ctx.arc(px, py, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = boardSkin.rim;
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    ctx.arc(px, py, r, 0, Math.PI * 2);
    ctx.stroke();
    ctx.strokeStyle = boardSkin.anchor;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(px, py, r * 0.76, 0, Math.PI * 2);
    ctx.stroke();
    // 一条朝向台面中心的短刻线，提示这是「面朝棋盘的棋手座位」。
    const length = Math.hypot(cx - px, cy - py) || 1;
    const ux = (cx - px) / length;
    const uy = (cy - py) / length;
    ctx.strokeStyle = boardSkin.rim;
    ctx.beginPath();
    ctx.moveTo(px + ux * r * 0.76, py + uy * r * 0.76);
    ctx.lineTo(px + ux * r * 1.3, py + uy * r * 1.3);
    ctx.stroke();
  }

  // 3) 概念图可选纹样：默认 marks=[]，台面就是一片干净的方格。
  const [lr, lg, lb] = boardSkin.line;
  const [br, bg, bb] = boardSkin.emblem;
  if (marks.includes("center_line")) {
    ctx.strokeStyle = `rgba(${lr}, ${lg}, ${lb}, 0.40)`;
    ctx.lineWidth = 1.3;
    const [x0, y0] = toPx(0, -hz * 0.9);
    const [x1, y1] = toPx(0, hz * 0.9);
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.stroke();
  }
  if (marks.includes("half_circles")) {
    for (const side of [-1, 1]) {
      const [cx, cy] = toPx(side * hx * 0.66, 0);
      ctx.strokeStyle = `rgba(${lr}, ${lg}, ${lb}, 0.32)`;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.arc(cx, cy, hz * 0.42 * scale, -Math.PI / 2, Math.PI / 2, side > 0);
      ctx.stroke();
    }
  }
  if (marks.includes("center_emblem")) {
    const [ex, ey] = toPx(0, 0);
    const er = Math.min(hx, hz) * 0.36 * scale;
    for (const [k, alpha] of [[1, 0.44], [0.78, 0.28], [0.42, 0.20]]) {
      ctx.strokeStyle = `rgba(${br}, ${bg}, ${bb}, ${alpha})`;
      ctx.lineWidth = k === 1 ? 1.6 : 1;
      ctx.beginPath();
      ctx.arc(ex, ey, er * k, 0, Math.PI * 2);
      ctx.stroke();
    }
  }
  ctx.restore();
}

// 3.png 的地面：深靛蓝底 + 成千上万短笔触 + 蓝白星点。
// 棋盘区域只是这块贴图上「略亮 + 有线」的一部分，笔触继续穿过，不挖空。
function makeGroundTexture(board) {
  const size = 1024;
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext("2d");
  const random = mulberry32(20261106);
  const projection = boardProjection(size);

  const ground = state.skin.ground;
  const gradient = ctx.createRadialGradient(size / 2, size / 2, 0, size / 2, size / 2, size * 0.52);
  gradient.addColorStop(0, ground.gradient[0]);
  gradient.addColorStop(0.55, ground.gradient[1]);
  gradient.addColorStop(1, ground.gradient[2]);
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, size, size);

  paintBoardBase(ctx, board, projection);

  const palette = ground.strokes;
  ctx.lineCap = "round";
  for (let i = 0; i < 6000; i += 1) {
    const x = random() * size;
    const y = random() * size;
    const r = Math.hypot(x - size / 2, y - size / 2) / (size / 2);
    if (r > 0.99) continue;
    // 中央只稍微稀疏一点，笔触必须连续穿过棋盘区域才算「同一片地面」。
    if (r < 0.20 && random() < 0.35) continue;
    const flow = Math.sin(x * 0.011 + y * 0.006) * 0.9
      + Math.cos(y * 0.017 - x * 0.004) * 0.5;
    const angle = flow + (random() - 0.5) * 0.9;
    const length = 5 + random() * 20;
    ctx.strokeStyle = palette[Math.floor(random() * palette.length)];
    ctx.globalAlpha = 0.05 + random() * 0.30;
    ctx.lineWidth = 0.7 + random() * 2.2;
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + Math.cos(angle) * length, y + Math.sin(angle) * length);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;

  paintBoardLines(ctx, board, projection);

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());
  return texture;
}

function makePathTexture() {
  const size = 256;
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext("2d");
  const random = mulberry32(4711);
  const ground = state.skin.ground;
  ctx.fillStyle = ground.path_base;
  ctx.fillRect(0, 0, size, size);
  const palette = ground.path_strokes;
  for (let i = 0; i < 900; i += 1) {
    const x = random() * size;
    const y = random() * size;
    ctx.globalAlpha = 0.2 + random() * 0.55;
    ctx.fillStyle = palette[Math.floor(random() * palette.length)];
    ctx.fillRect(x, y, 5 + random() * 22, 2 + random() * 6);
  }
  ctx.globalAlpha = 1;
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());
  return texture;
}

function makeRibbon(points, width, texture) {
  const curve = new THREE.CatmullRomCurve3(
    points.map(([x, z]) => new THREE.Vector3(x, 0, z)), false, "catmullrom", 0.5);
  const length = curve.getLength();
  const steps = Math.max(48, Math.ceil(length / 1.1));
  const positions = new Float32Array((steps + 1) * 6);
  const uvs = new Float32Array((steps + 1) * 4);
  const indices = [];
  for (let i = 0; i <= steps; i += 1) {
    const t = i / steps;
    const point = curve.getPointAt(t);
    const tangent = curve.getTangentAt(t);
    const nx = -tangent.z;
    const nz = tangent.x;
    const half = width / 2;
    const a = i * 6;
    positions[a] = point.x + nx * half;
    positions[a + 1] = 0.07;
    positions[a + 2] = point.z + nz * half;
    positions[a + 3] = point.x - nx * half;
    positions[a + 4] = 0.07;
    positions[a + 5] = point.z - nz * half;
    const b = i * 4;
    uvs[b] = 0;
    uvs[b + 1] = t * length / 7;
    uvs[b + 2] = 1;
    uvs[b + 3] = t * length / 7;
  }
  for (let i = 0; i < steps; i += 1) {
    const a = i * 2;
    indices.push(a, a + 2, a + 1, a + 1, a + 2, a + 3);
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("uv", new THREE.BufferAttribute(uvs, 2));
  geometry.setIndex(indices);
  return new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({
    map: texture, side: THREE.DoubleSide, toneMapped: false,
  }));
}

function buildGround(board) {
  const groundSkin = state.skin.ground;
  // 海面：盖住天穹球地平线以下被拉伸的部分，给夜晚一条干净的水平线。
  const sea = new THREE.Mesh(
    new THREE.CircleGeometry(900, 64),
    new THREE.MeshBasicMaterial({ color: groundSkin.sea, toneMapped: false }));
  sea.rotation.x = -Math.PI / 2;
  sea.position.y = -1.1;
  islandGroup.add(sea);

  // 岛屿：一整片圆形笔触地面；棋盘区域直接画在这张贴图里，没有独立几何。
  const ground = new THREE.Mesh(
    new THREE.CircleGeometry(ISLAND_R, 128),
    new THREE.MeshBasicMaterial({
      map: makeGroundTexture(board), side: THREE.DoubleSide, toneMapped: false,
    }));
  ground.rotation.x = -Math.PI / 2;
  ground.name = "starry_island";
  islandGroup.add(ground);

  // 岛缘的几层蓝色辉光，让圆形轮廓在夜色里立住（3.png 的边界感）。
  for (const [inner, outer, opacity] of [
    [ISLAND_R - 2.2, ISLAND_R, 0.34],
    [ISLAND_R, ISLAND_R + 5, 0.12],
    [ISLAND_R + 5, ISLAND_R + 14, 0.05],
  ]) {
    const ring = new THREE.Mesh(
      new THREE.RingGeometry(inner, outer, 128),
      new THREE.MeshBasicMaterial({
        color: groundSkin.rings, transparent: true, opacity,
        side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
      }));
    ring.rotation.x = -Math.PI / 2;
    ring.position.y = 0.02 + (outer - ISLAND_R) * 0.0005;
    islandGroup.add(ring);
  }

  // 蜿蜒小路：全部绕开 CLEAR_R，中央只留给画出来的棋盘区域。
  const pathTexture = makePathTexture();
  for (const path of PATHS) {
    islandGroup.add(makeRibbon(path.points, path.width, pathTexture));
  }
}

function buildFoliage() {
  const random = mulberry32(90210);
  const dummy = new THREE.Object3D();
  const foliage = state.skin.scenery;

  // 树丛：低面数多面体团簇，颜色由当前视觉包的 foliage_tones 决定。
  const treeCount = 72;
  const blobMaterial = new THREE.MeshStandardMaterial({
    color: 0xffffff, flatShading: true, roughness: 0.94, metalness: 0.0,
    emissive: foliage.foliage_emissive, emissiveIntensity: 0.5,
  });
  const blobs = new THREE.InstancedMesh(
    new THREE.IcosahedronGeometry(1, 1), blobMaterial, treeCount * 3);
  const trunks = new THREE.InstancedMesh(
    new THREE.CylinderGeometry(0.14, 0.24, 1, 5),
    new THREE.MeshStandardMaterial({ color: foliage.trunk, roughness: 1 }), treeCount);
  const blueTones = foliage.foliage_tones;
  for (let i = 0; i < treeCount; i += 1) {
    const angle = random() * Math.PI * 2;
    const radius = CLEAR_R + 3 + random() * (ISLAND_R - CLEAR_R - 7);
    const x = Math.cos(angle) * radius;
    const z = Math.sin(angle) * radius;
    const height = 1.9 + random() * 2.6;
    dummy.position.set(x, height * 0.62, z);
    dummy.rotation.set(0, 0, 0);
    dummy.scale.set(0.18, height * 0.62, 0.18);
    dummy.updateMatrix();
    trunks.setMatrixAt(i, dummy.matrix);
    for (let j = 0; j < 3; j += 1) {
      const r = 0.85 + random() * 0.75;
      dummy.position.set(
        x + (random() - 0.5) * 1.5,
        height + (random() - 0.5) * 0.9,
        z + (random() - 0.5) * 1.5);
      dummy.rotation.set(random() * 0.6, random() * Math.PI, random() * 0.4);
      dummy.scale.set(r, r * (0.8 + random() * 0.5), r);
      dummy.updateMatrix();
      const index = i * 3 + j;
      blobs.setMatrixAt(index, dummy.matrix);
      blobs.setColorAt(index, new THREE.Color(blueTones[Math.floor(random() * blueTones.length)]));
    }
  }
  blobs.instanceMatrix.needsUpdate = true;
  if (blobs.instanceColor) blobs.instanceColor.needsUpdate = true;
  trunks.instanceMatrix.needsUpdate = true;
  blobs.frustumCulled = false;
  trunks.frustumCulled = false;
  islandGroup.add(blobs, trunks);

  // 柏树：2.png 左侧那几株近乎黑色的尖塔剪影。
  const cypressCount = 16;
  const cypressSites = [];
  const cypresses = new THREE.InstancedMesh(
    new THREE.ConeGeometry(1, 1, 7),
    new THREE.MeshStandardMaterial({
      color: foliage.cypress, flatShading: true, roughness: 1,
      emissive: foliage.cypress_emissive, emissiveIntensity: 0.35,
    }), cypressCount);
  for (let i = 0; i < cypressCount; i += 1) {
    const angle = random() * Math.PI * 2;
    const radius = CLEAR_R + 5 + random() * (ISLAND_R - CLEAR_R - 10);
    const height = 8 + random() * 9;
    dummy.position.set(Math.cos(angle) * radius, height / 2, Math.sin(angle) * radius);
    cypressSites.push([Math.cos(angle) * radius, Math.sin(angle) * radius, height]);
    dummy.rotation.set(0, random() * Math.PI, (random() - 0.5) * 0.12);
    dummy.scale.set(1.1 + random() * 0.9, height, 1.1 + random() * 0.9);
    dummy.updateMatrix();
    cypresses.setMatrixAt(i, dummy.matrix);
  }
  cypresses.instanceMatrix.needsUpdate = true;
  cypresses.frustumCulled = false;
  islandGroup.add(cypresses);

  // 屋舍 + 亮窗：远景村庄剪影，窗口用 >1 的 HDR 颜色让 bloom 抓到。
  const houses = [];
  for (const [count, baseAngle, spread, rMin, rMax] of [
    [13, -Math.PI / 2, 0.85, 27, 42],
    [11, Math.PI / 2, 0.95, 26, 43],
  ]) {
    for (let i = 0; i < count; i += 1) {
      houses.push({
        angle: baseAngle + (random() - 0.5) * spread * 2,
        radius: rMin + random() * (rMax - rMin),
        w: 2.0 + random() * 2.0,
        h: 2.2 + random() * 2.4,
        d: 2.0 + random() * 2.0,
      });
    }
  }
  const bodies = new THREE.InstancedMesh(
    new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({
      color: foliage.house, roughness: 0.96, metalness: 0,
      emissive: foliage.house_emissive, emissiveIntensity: 0.45,
    }), houses.length);
  const roofs = new THREE.InstancedMesh(
    new THREE.ConeGeometry(1, 1, 4),
    new THREE.MeshStandardMaterial({
      color: foliage.roof, flatShading: true, roughness: 0.95,
    }), houses.length);
  const windowGeometry = new THREE.PlaneGeometry(0.34, 0.42);
  const windows = new THREE.InstancedMesh(
    windowGeometry,
    new THREE.MeshBasicMaterial({
      color: new THREE.Color().setRGB(...foliage.window),
      side: THREE.DoubleSide, toneMapped: false,
    }), houses.length * 2);
  houses.forEach((house, i) => {
    const x = Math.cos(house.angle) * house.radius;
    const z = Math.sin(house.angle) * house.radius;
    dummy.rotation.set(0, 0, 0);
    dummy.position.set(x, house.h / 2, z);
    dummy.scale.set(house.w, house.h, house.d);
    dummy.updateMatrix();
    bodies.setMatrixAt(i, dummy.matrix);
    dummy.position.y = house.h + house.w * 0.28;
    dummy.rotation.y = Math.PI / 4;
    dummy.scale.set(house.w * 0.82, house.w * 0.58, house.d * 0.82);
    dummy.updateMatrix();
    roofs.setMatrixAt(i, dummy.matrix);
    for (let w = 0; w < 2; w += 1) {
      dummy.rotation.set(0, 0, 0);
      dummy.position.set(x + (w ? 0.45 : -0.45) * house.w,
        house.h * 0.56, z + house.d / 2 + 0.012);
      dummy.scale.set(1, 1, 1);
      dummy.updateMatrix();
      windows.setMatrixAt(i * 2 + w, dummy.matrix);
    }
  });
  for (const mesh of [bodies, roofs, windows]) {
    mesh.instanceMatrix.needsUpdate = true;
    mesh.frustumCulled = false;
    islandGroup.add(mesh);
  }

  // 白色笔触：散落在地面的短亮条（2.png 近景 / 3.png 全岛都有）。
  const strokeCount = 320;
  const strokeGeometry = new THREE.PlaneGeometry(1, 1);
  strokeGeometry.rotateX(-Math.PI / 2);
  const strokes = new THREE.InstancedMesh(
    strokeGeometry,
    new THREE.MeshBasicMaterial({
      color: state.skin.ground.white_stroke, transparent: true, opacity: 0.72,
      side: THREE.DoubleSide, depthWrite: false, toneMapped: false,
    }), strokeCount);
  for (let i = 0; i < strokeCount; i += 1) {
    const angle = random() * Math.PI * 2;
    const radius = CLEAR_R - 2 + random() * (ISLAND_R - CLEAR_R - 1);
    dummy.position.set(Math.cos(angle) * radius, 0.05, Math.sin(angle) * radius);
    dummy.rotation.set(0, random() * Math.PI, 0);
    dummy.scale.set(0.35 + random() * 1.5, 1, 0.08 + random() * 0.16);
    dummy.updateMatrix();
    strokes.setMatrixAt(i, dummy.matrix);
  }
  strokes.instanceMatrix.needsUpdate = true;
  strokes.frustumCulled = false;
  islandGroup.add(strokes);
  return { houses, cypressSites };
}

// starry / generic 的地标由 buildFoliage 生成：用它的真实锚点登记，
// 与 world-scenery.mjs 的 feature() 汇入同一份 state.features。
function registerStarryFeatures(anchors) {
  const village = anchors.houses[0];
  const [cx, cz, height] = anchors.cypressSites[0];
  registerFeature("starry_village", "村庄灯火", "building",
    [Math.cos(village.angle) * village.radius, village.h * 0.6,
      Math.sin(village.angle) * village.radius], { weight: 3 });
  registerFeature("starry_cypress", "柏树剪影", "grove",
    [cx, height, cz], { weight: 3 });
  registerFeature("starry_lantern", "路边星灯", "lantern",
    [CLEAR_R + 6, 2, -(CLEAR_R - 10)], { weight: 2 });
  registerFeature("starry_sky", "旋转的夜空", "sky",
    [0, 9, 0], { weight: 2, shot: { r: 30, el: -2 } });
}

// ---------------------------------------------------------------- 场景物件
// 4.png 里场地边上是篮球架 / 长椅 / 球车这类「场景物件」，不是围着棋盘摆一圈
// 的金色装饰。这里用与树丛 / 屋舍同一套冷蓝石质语汇，沿小路散布石灯 + 矮石座。
function buildSceneProps() {
  const random = mulberry32(13579);
  const props = state.skin.scenery.props;
  const stone = new THREE.MeshStandardMaterial({
    color: props.stone, roughness: 0.95, metalness: 0.02,
    emissive: state.skin.scenery.house_emissive, emissiveIntensity: 0.5, flatShading: true,
  });
  const wick = new THREE.MeshBasicMaterial({
    color: new THREE.Color().setRGB(...props.wick), toneMapped: false,
  });
  const postGeometry = new THREE.CylinderGeometry(0.11, 0.20, 1.45, 6);
  const capGeometry = new THREE.SphereGeometry(0.20, 8, 6);
  const baseGeometry = new THREE.CylinderGeometry(0.30, 0.34, 0.16, 6);
  let lanterns = 0;

  const addLantern = (x, z) => {
    const group = new THREE.Group();
    const base = new THREE.Mesh(baseGeometry, stone);
    base.position.y = 0.08;
    const post = new THREE.Mesh(postGeometry, stone);
    post.position.y = 0.85;
    const cap = new THREE.Mesh(capGeometry, wick);
    cap.position.y = 1.68;
    cap.userData = {
      id: `${props.id_prefix}_${++lanterns}`,
      tag: props.tag,
      hint: props.hint,
      kind: KIND.INTERACTABLE,
    };
    group.add(base, post, cap);
    group.position.set(x, 0, z);
    scenePropsGroup.add(group);
    state.interactive.push(cap);
  };

  for (const path of PATHS) {
    const curve = new THREE.CatmullRomCurve3(
      path.points.map(([x, z]) => new THREE.Vector3(x, 0, z)), false, "catmullrom", 0.5);
    for (let i = 0; i < 6; i += 1) {
      const t = 0.08 + (i / 5) * 0.84;
      const point = curve.getPointAt(t);
      const tangent = curve.getTangentAt(t);
      const side = (i % 2 ? 1 : -1) * (path.width / 2 + 1.25 + random() * 0.6);
      const x = point.x - tangent.z * side;
      const z = point.z + tangent.x * side;
      if (Math.hypot(x, z) < CLEAR_R + 1.5) continue;
      addLantern(x, z);
    }
  }

  // 矮石座散在树丛之间，高度压得很低，不抢画出来的棋盘区域。
  const benches = new THREE.InstancedMesh(
    new THREE.BoxGeometry(1.25, 0.26, 0.48), stone, 16);
  const dummy = new THREE.Object3D();
  for (let i = 0; i < 16; i += 1) {
    const angle = random() * Math.PI * 2;
    const radius = CLEAR_R + 3 + random() * (ISLAND_R - CLEAR_R - 9);
    dummy.position.set(Math.cos(angle) * radius, 0.16, Math.sin(angle) * radius);
    dummy.rotation.set(0, random() * Math.PI, 0);
    dummy.scale.set(0.8 + random() * 0.5, 1, 1);
    dummy.updateMatrix();
    benches.setMatrixAt(i, dummy.matrix);
  }
  benches.instanceMatrix.needsUpdate = true;
  benches.frustumCulled = false;
  scenePropsGroup.add(benches);
}

// ---------------------------------------------------------------- 星尘
function buildParticles(spec) {
  const random = mulberry32(spec.sky.star_seed);
  const starGeometry = new THREE.BufferGeometry();
  const starCount = 620;
  const positions = new Float32Array(starCount * 3);
  for (let i = 0; i < starCount; i += 1) {
    const u = random() * 0.86 + 0.02;
    const theta = random() * Math.PI * 2;
    const radius = 230;
    const flat = Math.sqrt(1 - u * u);
    positions[i * 3] = radius * flat * Math.cos(theta);
    positions[i * 3 + 1] = radius * u;
    positions[i * 3 + 2] = radius * flat * Math.sin(theta);
  }
  starGeometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  const particles = state.skin.scenery.particles;
  const stars = new THREE.Points(starGeometry, new THREE.PointsMaterial({
    color: new THREE.Color().setRGB(...particles.star),
    size: 1.6, sizeAttenuation: false, transparent: true, opacity: particles.star_opacity,
    blending: THREE.AdditiveBlending, depthWrite: false, fog: false,
  }));
  stars.name = "sky_stars";
  // 白昼/黄昏世界没有星空：保留 Points 以便「流动星尘」开关语义不变，但不渲染。
  stars.visible = particles.star_opacity > 0;
  fxGroup.add(stars);

  const dustCount = 520;
  const dustGeometry = new THREE.BufferGeometry();
  const dustPositions = new Float32Array(dustCount * 3);
  for (let i = 0; i < dustCount; i += 1) {
    const angle = random() * Math.PI * 2;
    const radius = random() * (ISLAND_R + 10);
    dustPositions[i * 3] = Math.cos(angle) * radius;
    dustPositions[i * 3 + 1] = 0.6 + random() * 16;
    dustPositions[i * 3 + 2] = Math.sin(angle) * radius;
  }
  dustGeometry.setAttribute("position", new THREE.BufferAttribute(dustPositions, 3));
  // 白昼世界的加色浮尘会变成满屏白点：密度跟着星空强度走，星夜仍是 0.8，
  // 全无星的白昼降到 0.25（星空 Points 本身已由 star_opacity 关掉）。
  const dustOpacity = 0.25 + 0.55 * state.skin.sky.stars;
  dust = new THREE.Points(dustGeometry, new THREE.PointsMaterial({
    color: particles.dust, size: 0.16, sizeAttenuation: true,
    transparent: true, opacity: dustOpacity, blending: THREE.AdditiveBlending,
    depthWrite: false, fog: true,
  }));
  dust.name = "floating_stardust";
  // 没有星空的世界直接不渲染浮尘：AdditiveBlending 的白色小点会被 bloom
  // 放大成「白昼下雪」。`toggle-stars` 控制整个 fxGroup，语义不受影响。
  dust.visible = state.skin.sky.stars > 0.05;
  fxGroup.add(dust);
}

// ---------------------------------------------------------------- 灯光 / 后处理
function addLights(visual) {
  const base = state.spec.lighting;
  const light = { ...base, ...visual.lighting };
  // r155+ 物理灯光按 lux 解释，Unity / 旧语义的数值要 ×π 才等价
  //（不要用已废弃的 renderer.useLegacyLights）。
  const LUX_SCALE = Math.PI;
  const e = light.directional_euler;
  // 全部灯光登记下来：入场演出会按进度把它们由暗渐明，结束再恢复原亮度。
  const lights = [];
  const directional = new THREE.DirectionalLight(
    srgb(light.directional_color), light.directional_intensity * LUX_SCALE);
  directional.position.set(
    Math.sin(e[1] * DEG) * 40,
    Math.max(12, Math.cos(e[0] * DEG) * 55),
    Math.cos(e[1] * DEG) * 40);
  scene.add(directional);
  lights.push(directional);
  const ambient = new THREE.AmbientLight(
    srgb(light.ambient_color), light.ambient_intensity * LUX_SCALE);
  scene.add(ambient);
  lights.push(ambient);
  const lighting = state.skin.lighting;
  const hemisphere = new THREE.HemisphereLight(
    lighting.hemisphere[0], lighting.hemisphere[1], 0.55 * LUX_SCALE);
  scene.add(hemisphere);
  lights.push(hemisphere);

  // 棋盘区域四角的灯池，让画出来的场地线与周围树丛有近景层次；亮度按画境定调。
  for (const [x, z] of [[-11, -9], [11, -9], [-11, 9], [11, 9]]) {
    const lamp = new THREE.PointLight(
      lighting.lamps, lighting.lamp_intensity * LUX_SCALE, 26, 2);
    lamp.position.set(x, 3.2, z);
    scene.add(lamp);
    lights.push(lamp);
  }
  state.entranceLights = lights;
}

async function setupPost(visual) {
  const bloom = visual.post_processing.bloom;
  renderer.toneMappingExposure = 1 + visual.post_processing.exposure_value;
  const fog = visual.skybox.fog;
  if (fog.enabled) {
    scene.fog = new THREE.FogExp2(srgb(fog.color), fog.density);
  }
  try {
    const [composerModule, renderModule, bloomModule, outputModule] = await Promise.all([
      import("three/addons/postprocessing/EffectComposer.js"),
      import("three/addons/postprocessing/RenderPass.js"),
      import("three/addons/postprocessing/UnrealBloomPass.js"),
      import("three/addons/postprocessing/OutputPass.js"),
    ]);
    composer = new composerModule.EffectComposer(renderer);
    composer.addPass(new renderModule.RenderPass(scene, camera));
    bloomPass = new bloomModule.UnrealBloomPass(
      new THREE.Vector2(innerWidth, innerHeight),
      bloom.intensity, bloom.diffusion, bloom.threshold);
    bloomPass.enabled = !!bloom.enabled;
    composer.addPass(bloomPass);
    composer.addPass(new outputModule.OutputPass());
    composer.setSize(innerWidth, innerHeight);
    state.postAvailable = true;
  } catch (error) {
    // CDN 拿不到 addons 时降级为直出，场景本身照常可见。
    composer = null;
    bloomPass = null;
    state.postAvailable = false;
    console.warn("[ZEngine] post-processing unavailable, direct render fallback", error);
  }
}

// ---------------------------------------------------------------- 棋盘区域
// Andrew monotone chain：把 core_slab 顶/底面顶点的 XZ 投影取凸包，得到冻结
// 契约里的倒角矩形轮廓（8 个点）。Web 不写任何棋盘公式，只消费 ZBRD 字节。
function convexHullXZ(positions) {
  const points = [];
  const seen = new Set();
  for (let i = 0; i < positions.length; i += 3) {
    const x = Math.round(positions[i] * 10000) / 10000;
    const z = Math.round(positions[i + 2] * 10000) / 10000;
    const key = `${x},${z}`;
    if (seen.has(key)) continue;
    seen.add(key);
    points.push([x, z]);
  }
  points.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (o, a, b) =>
    (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  // 扇形三角化会在边界留下近共线点，容差把它们剔掉；倒角本身是多段的，
  // 所以结果仍是「倒角矩形」的多点轮廓（实测 16 点），不是 4 个尖角。
  const lower = [];
  for (const p of points) {
    while (lower.length >= 2
      && cross(lower[lower.length - 2], lower[lower.length - 1], p) <= 1e-4) lower.pop();
    lower.push(p);
  }
  const upper = [];
  for (let i = points.length - 1; i >= 0; i -= 1) {
    const p = points[i];
    while (upper.length >= 2
      && cross(upper[upper.length - 2], upper[upper.length - 1], p) <= 1e-4) upper.pop();
    upper.push(p);
  }
  lower.pop();
  upper.pop();
  return lower.concat(upper);
}

function partBoundsXZ(part) {
  let minX = Infinity;
  let maxX = -Infinity;
  let minZ = Infinity;
  let maxZ = -Infinity;
  for (let i = 0; i < part.positions.length; i += 3) {
    minX = Math.min(minX, part.positions[i]);
    maxX = Math.max(maxX, part.positions[i]);
    minZ = Math.min(minZ, part.positions[i + 2]);
    maxZ = Math.max(maxZ, part.positions[i + 2]);
  }
  return {
    cx: (minX + maxX) / 2, cz: (minZ + maxZ) / 2,
    rx: (maxX - minX) / 2, rz: (maxZ - minZ) / 2,
  };
}

// 只解析，不建 mesh：棋盘是地形贴图上的一块区域，ZBRD 只提供轮廓 / 锚点 /
// 逻辑边界。sky@HD 等部件一概不进场景，天穹交给 panorama。
async function loadBoardRegion() {
  const [boardBytes, manifest] = await Promise.all([
    fetchBytes(BUILD + "board.zbrd"),
    fetchJSON(BUILD + "board.manifest.json"),
  ]);
  state.manifest = manifest;
  for (const item of manifest.interactables ?? []) {
    state.hints.set(item.tag, item.hint);
    state.hints.set(item.id, item.hint);
  }
  const parts = filterByLod(parseZBRD(boardBytes), "HD");
  const slab = parts.find((part) => part.name === "core_slab" && part.kind === KIND.GRID);
  if (!slab) throw new Error("ZBRD HD 档缺少 core_slab");
  const outline = convexHullXZ(slab.positions);
  if (outline.length < 4) throw new Error("core_slab 轮廓不是凸多边形");
  const slabBounds = partBoundsXZ(slab);
  const anchors = parts
    .filter((part) => part.name.startsWith("anchor_"))
    .map((part) => {
      const bounds = partBoundsXZ(part);
      return [bounds.cx, bounds.cz, Math.max(bounds.rx, bounds.rz)];
    });
  return {
    outline,
    half: [slabBounds.rx, slabBounds.rz],
    anchors,
    logical: manifest.grid?.logical_bounds_xz ?? null,
    partCount: parts.length,
  };
}

async function loadExternalMeshes(visual) {
  let manifest;
  try {
    manifest = await fetchJSON(BUILD + "external_mesh.manifest.json");
  } catch {
    return; // 没有清单就只留画在地面上的棋盘区域
  }
  state.ext = manifest;
  if (manifest.schema !== EXTERNAL_MANIFEST_SCHEMA) {
    state.external.error = "清单 schema 不匹配";
    return;
  }
  state.external.total = (manifest.placements ?? []).length;
  const { assets, groups } = groupPlacements(manifest);
  const glow = srgb(visual.palette.glow);

  for (const [assetId, placements] of groups) {
    const asset = assets.get(assetId);
    try {
      // Unity 读的就是这份索引网格缓存；校验 cache_sha256 后直接复用顶点表。
      const cacheBytes = await fetchBytes("../" + asset.cache);
      if (!(await verifySha256(cacheBytes, asset.cache_sha256))) {
        throw new Error("cache sha256 校验失败");
      }
      const [part] = parseZBRD(cacheBytes);
      if (!part) throw new Error("cache 中没有部件");
      const geometry = makeGeometry(part);

      let texture = null;
      if (asset.texture && asset.texture_sha256) {
        try {
          texture = await loadShaTexture(asset.texture, asset.texture_sha256);
          texture.colorSpace = THREE.SRGBColorSpace;
          texture.flipY = false; // glTF TEXCOORD_0 左上原点
          texture.needsUpdate = true;
          state.external.textured += 1;
        } catch (error) {
          texture = null;
          state.external.error = `${assetId}: ${error.message}`;
          console.warn("[ZEngine] baseColor texture skipped", assetId, error);
        }
      }
      const fallback = srgb(visual.materials?.[placements[0].material]?.color
        ?? part.color ?? [0.8, 0.8, 0.8, 1]);
      const material = new THREE.MeshStandardMaterial({
        // baseColor = texture × baseColorFactor，factor 恒为白，别再乘代理纯色。
        map: texture,
        color: texture ? new THREE.Color(1, 1, 1) : fallback,
        roughness: 0.42, metalness: 0.32, side: THREE.DoubleSide,
        emissive: glow.clone().multiplyScalar(0.55),
        emissiveIntensity: 1,
      });

      for (const placement of placements) {
        // 场景里已无 ZBRD 代理部件，单体直接按同一份锚点立在地上。
        const mesh = new THREE.Mesh(geometry, material);
        applyTransform(mesh, placement, asset.pivot_offset);
        mesh.name = `ext_${placement.instance_id}`;
        mesh.userData = {
          id: placement.instance_id, tag: placement.tag,
          kind: KIND.INTERACTABLE, external: true,
        };
        boardGroup.add(mesh);
        state.interactive.push(mesh);
        state.external.applied += 1;
      }
    } catch (error) {
      state.external.error = `${assetId}: ${error.message}`;
      console.warn("[ZEngine] external mesh skipped", assetId, error);
    }
  }
}

// ---------------------------------------------------------------- 交互
function setView(mode) {
  const preset = PRESETS[mode];
  if (!preset) return;
  cancelEntranceOrbit();
  viewMode = mode;
  tween = { from: { ...cam }, to: tunedPreset(mode), start: performance.now(), duration: 1150 };
  $("view-immersive").classList.toggle("active", mode === "immersive");
  $("view-overview").classList.toggle("active", mode === "overview");
}

function showInspect(object) {
  const box = $("inspect");
  box.hidden = !object;
  if (!object) return;
  const data = object.userData ?? {};
  $("inspect-id").textContent = data.id ?? "—";
  $("inspect-tag").textContent = data.tag
    || `${state.pack.title_en} / 场景物件`;
  // 场景物件自带场景化描述；混元单体（可选陈列）才回退到清单里的蓝图 hint。
  $("inspect-hint").textContent = data.hint
    ?? state.hints.get(data.tag)
    ?? state.hints.get(data.id)
    ?? `${state.pack.title_zh}里的一处灯火。拖动环视，滚轮推进，或切到俯瞰看整座岛。`;
}

// 6 类演出各自的氛围参数：fov 是渲染层的临时广角偏移（不写回 cam），
// bloom 是强度增量，dust 是星尘附加转速。数值只影响观感，不进任何契约。
// entrance 不走 pulse：它有自己的环绕镜头，fov / bloom 由 driveEntranceFx 接管。
const CEREMONY_FX = {
  entrance:   { fov: 12, bloom: 0.35, dust: 0.0006 },
  round_win:  { fov: -2, bloom: 0.80, dust: 0.0012 },
  round_lose: { fov: 4, bloom: -0.28, dust: 0.0 },
  win_streak: { fov: -3, bloom: 1.00, dust: 0.0016 },
  level_up:   { fov: -4, bloom: 0.70, dust: 0.0008 },
  easter_egg: { fov: 3, bloom: 0.55, dust: 0.0020 },
};

// ---------------------------------------------------------------- 入场环绕
// 镜头由场景自己长出来：world-scenery.mjs 与 buildFoliage 在生成地标时把
// {label, kind, focus, weight, shot} 登记进 state.features（focus 与几何共用
// 同一批变量 / 数组），这里只按 kind 查机位语言、按 weight 选站点，自动排出
// 分镜。挪一个桥、加一个画境，镜头跟着变，不再维护第二份手写坐标表。
//
// 机位语言是共享的镜头语法：桥退远看全、水面贴低横移、建筑中近景、山脊
// 仰视、树丛贴地、棋盘低空俯视；地标可用 shot 覆盖个别参数做特性化。
// 运动刻意克制：短弧幅度由主地标的 arc 决定（30~54°），方向由它在岛上的
// 方位决定，帧间 smoothstep 不过冲；末帧落回 tunedPreset(viewMode)。
// 帧字段：az / el 用度，写进 cam.azimuth 时要 * DEG（轨道相机用弧度）。
// 登记辅助：starry 内建画境（柏树 / 村庄 / 石灯 / 夜空）走这里，
// 其余 12 个 preset 由 world-scenery.mjs 的 feature() 登记。
function registerFeature(id, label, kind, focus, extra = {}) {
  state.features.push({ id, label, kind, focus, weight: 1, ...extra });
}

// kind -> 机位语言（共享的镜头语法）。subject 同时用作横幅的「突出 X」。
// 地标可用 shot 覆盖个别参数：桥要退远看全、山要仰视、水面要贴低。
const SHOT_LANGUAGE = {
  bridge:   { subject: "桥",   r: 32, el: 8,  fov: 52, arc: 20 },
  water:    { subject: "河流", r: 30, el: 4,  fov: 55, arc: 16 },
  building: { subject: "建筑", r: 32, el: 11, fov: 52, arc: 16 },
  interior: { subject: "室内", r: 29, el: 10, fov: 54, arc: 14 },
  mountain: { subject: "山峰", r: 44, el: -4, fov: 46, arc: 12 },
  grove:    { subject: "树丛", r: 31, el: 9,  fov: 54, arc: 16 },
  field:    { subject: "田野", r: 30, el: 6,  fov: 54, arc: 16 },
  flag:     { subject: "旗帜", r: 32, el: 12, fov: 50, arc: 14 },
  lantern:  { subject: "灯火", r: 30, el: 7,  fov: 55, arc: 12 },
  sky:      { subject: "天光", r: 34, el: -2, fov: 56, arc: 10 },
  path:     { subject: "小路", r: 29, el: 5,  fov: 57, arc: 14 },
  board:    { subject: "棋盘", r: 27, el: 12, fov: 58, arc: 8  },
};

function languageOf(item) {
  return { ...SHOT_LANGUAGE.board, ...SHOT_LANGUAGE[item.kind],
    ...(item.shot ?? {}) };
}

function bearingOf(focus) {
  return Math.atan2(focus[0], focus[2]) / DEG;
}

function angleDelta(a, b) {
  return ((a - b + 540) % 360) - 180;
}

// 从登记的特色自动生成 5 帧：建立镜头 -> 绕主锚点 -> 收回棋盘 -> 落位。
//
// 相机位置与注视点解耦：updateCamera 按「棋盘中心 + az/radius/el」算稳定
// 短弧，注视点 look 指向登记地标，在 aimEntranceCamera 里覆盖 lookAt。
//
// 注视点沿「主锚点 -> 棋盘中心」同一条射线单调收缩（keeps 1→0），整段只
// 看一个特色、方向不变；相机从锚点对面走 ≤45° 短弧绕回默认机位。上一版
// 在多个地标之间切注视点 + 绕 100°，实测摇镜 40~57°，那就是「乱」的来源。
function deriveEntranceFrames() {
  const registered = (state.features ?? [])
    .filter((item) => item && Array.isArray(item.focus));
  const outline = state.board?.outline ?? [];
  const home = tunedPreset(viewMode);
  const pivot = [0, home.targetY, 0];
  const boardFocus = [home.panX, home.targetY, home.panZ];
  const boardLook = outline.length
    ? [outline.reduce((sum, p) => sum + p[0], 0) / outline.length, 0,
      outline.reduce((sum, p) => sum + p[1], 0) / outline.length]
    : [0, 0, 0];
  const board = {
    id: "board", label: "画境棋盘", kind: "board", focus: boardFocus, weight: 0,
  };

  // 主锚点 = 权重最高的登记地标，机位语言由它的 kind（+ shot 覆盖）决定。
  const ranked = [...registered].sort((a, b) => (b.weight ?? 1) - (a.weight ?? 1));
  const main = ranked[0] ?? board;
  const lang = languageOf(main);
  const mainBearing = bearingOf(main.focus);
  const anchorRadius = Math.hypot(main.focus[0], main.focus[2]);
  const opposite = anchorRadius < 5 ? 0 : angleDelta(mainBearing + 180, 0);
  const rawSweep = THREE.MathUtils.clamp(opposite, -45, 45);
  const sweep = Math.abs(rawSweep) < 20
    ? 20 * (mainBearing >= 0 ? 1 : -1) : rawSweep;

  // keeps 是注视点在主锚点->棋盘射线上的位置，1 = 真实地标坐标（抓到锚点）。
  const keeps = [1, 0.7, 0.35, 0.1, 0];
  const looks = keeps.map((keep) => main.focus.map(
    (v, i) => v * keep + boardLook[i] * (1 - keep)));
  looks[4] = boardFocus;   // 落位帧注视点回到默认构图
  const ats = [0, 0.24, 0.48, 0.74, 1];
  const azs = [1, 0.75, 0.45, 0.15].map((f) => sweep * f);
  azs.push(home.azimuth / DEG);   // 末帧落回默认方位（弧度 -> 度）
  const rs = [52, lang.r, lang.r * 0.92, 28, home.radius];
  const els = [18, lang.el, lang.el * 0.7 + 2, 8, home.elevation];
  const fovs = [44, lang.fov, lang.fov + 2, 58, home.fov];
  const labels = ["画境远景", main.label, `${main.label} · 环绕`,
    "画境棋盘", "画境展开"];
  const subjects = ["全景", lang.subject, lang.subject, "棋盘", "棋盘"];
  return ats.map((at, i) => ({
    at, label: labels[i], subject: subjects[i],
    focus: pivot, look: looks[i],
    az: azs[i], r: rs[i], el: els[i], fov: fovs[i],
  }));
}

function easeInOutCubic(t) {
  return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
}

// 分段 smoothstep：每段首尾速度为 0，镜头在每处特色上稳一下再走，不会像
// Catmull-Rom 那样在端点过冲、把方位甩过头造成「疯狂转圈」。
function sampleEntranceFrame(frames, t) {
  let i = 0;
  while (i < frames.length - 2 && t > frames[i + 1].at) i += 1;
  const a = frames[i];
  const b = frames[i + 1];
  const u = THREE.MathUtils.clamp(
    (t - a.at) / Math.max(1e-6, b.at - a.at), 0, 1);
  const s = u * u * (3 - 2 * u);
  const mix = (get) => THREE.MathUtils.lerp(get(a), get(b), s);
  const axis = (k) => (frame) => frame.focus[k];
  const lookAxis = (k) => (frame) => frame.look[k];
  return {
    focus: [mix(axis(0)), mix(axis(1)), mix(axis(2))],
    look: [mix(lookAxis(0)), mix(lookAxis(1)), mix(lookAxis(2))],
    az: mix((frame) => frame.az),
    r: mix((frame) => frame.r),
    el: mix((frame) => frame.el),
    fov: mix((frame) => frame.fov),
  };
}

// 末帧 home 已由 deriveEntranceFrames 按 tunedPreset(viewMode) 填好。
function makeEntranceFrames() {
  return deriveEntranceFrames();
}

// 入场光效：棋盘区域轮廓描边「亮起」+ 从中心扩散的光圈 + 灯光由暗渐明。
// 轮廓取 state.board.outline（ZBRD 冻结契约），不另写一套棋盘公式。
function buildEntranceFx() {
  const accent = new THREE.Color(
    state.skin.scenery.ceremony?.accent
    ?? ceremonyDoc?.palette_defaults?.accent ?? "#e8b96a");
  const group = new THREE.Group();
  group.name = "entrance_fx";
  group.visible = false;
  const points = state.board.outline.map(([x, z]) => new THREE.Vector3(x, 0.07, z));
  const line = new THREE.LineLoop(
    new THREE.BufferGeometry().setFromPoints(points),
    new THREE.LineBasicMaterial({
      color: accent, transparent: true, opacity: 0,
      depthWrite: false, toneMapped: false,
    }));
  group.add(line);
  const ring = new THREE.Mesh(
    new THREE.RingGeometry(0.96, 1, 96),
    new THREE.MeshBasicMaterial({
      color: accent, transparent: true, opacity: 0, side: THREE.DoubleSide,
      depthWrite: false, toneMapped: false, blending: THREE.AdditiveBlending,
    }));
  ring.rotation.x = -Math.PI / 2;
  ring.position.y = 0.09;
  group.add(ring);
  scene.add(group);
  entranceFx = {
    group, line, ring,
    lights: (state.entranceLights ?? []).map((light) => ({ light, base: light.intensity })),
    bloomBase: bloomPass?.strength ?? 0,
  };
}

function driveEntranceOrbit(t) {
  const frame = sampleEntranceFrame(entranceShot.frames, t);
  // focus 只决定相机位置（固定绕棋盘中心的稳定短弧），不决定注视点。
  cam.panX = frame.focus[0];
  cam.targetY = frame.focus[1];
  cam.panZ = frame.focus[2];
  cam.azimuth = frame.az * DEG;   // 分镜表用度，轨道相机用弧度
  cam.radius = frame.r;
  cam.elevation = frame.el;
  cam.fov = frame.fov;
  // 真实地标坐标交给 aimEntranceCamera 覆盖 camera.lookAt，镜头才真的对准锚点。
  entranceShot.look = frame.look;
}

// 入场第一帧就得对准锚点：startEntranceOrbit 里的 updateCamera 只会
// lookAt 棋盘中心，不能等下一帧 animate 才纠正（会闪一帧错误朝向）。
// Camera.lookAt 从 matrixWorld 取 eye，推进快时先同步本帧矩阵。
function aimEntranceCamera() {
  if (!entranceShot?.look) return;
  const [lx, ly, lz] = entranceShot.look;
  camera.updateMatrixWorld();
  camera.lookAt(lx, ly, lz);
}

function driveEntranceFx(t) {
  const fx = entranceFx;
  if (!fx) return;
  const env = Math.sin(Math.PI * t);
  fx.group.visible = true;
  fx.line.material.opacity = 0.9 * env;
  fx.ring.scale.setScalar(2 + 24 * t);
  fx.ring.material.opacity = 0.5 * (1 - t)
    * THREE.MathUtils.smoothstep(t, 0, 0.14);
  const lit = easeInOutCubic(Math.min(1, t * 1.2));
  for (const entry of fx.lights) {
    entry.light.intensity = entry.base * (0.38 + 0.62 * lit);
  }
  if (bloomPass) bloomPass.strength = fx.bloomBase + CEREMONY_FX.entrance.bloom * env;
}

function updateEntranceShot(time) {
  const shot = entranceShot;
  if (!shot) return;
  const t = THREE.MathUtils.clamp((time - shot.start) / shot.duration, 0, 1);
  driveEntranceOrbit(t);
  driveEntranceFx(t);
  if (dust) dust.rotation.y += CEREMONY_FX.entrance.dust * Math.sin(Math.PI * t) * 16;
  const stage = Math.min(shot.labels.length - 1, Math.floor(t * shot.labels.length));
  if (stage !== shot.stage) {
    shot.stage = stage;
    ceremony?.stage?.(
      `ORBIT 0${stage + 1} / 0${shot.labels.length} · 突出${shot.subjects[stage]}`,
      `${shot.labels[stage]} · ${state.pack.title_zh}`);
  }
  if (t >= 1) settleEntrance();
}

// 用户主动操作时把镜头交还给他：cam 停在当前机位，光效立刻收干净。
function cancelEntranceOrbit() {
  if (!entranceShot) return;
  entranceShot = null;
  if (entranceFx) {
    entranceFx.group.visible = false;
    entranceFx.line.material.opacity = 0;
    entranceFx.ring.material.opacity = 0;
    for (const entry of entranceFx.lights) entry.light.intensity = entry.base;
    if (bloomPass) bloomPass.strength = entranceFx.bloomBase;
  }
}

// 把构图精确复位到当前视角预设：演出收尾与无头探针取基线都走这里，
// 所以自动入场可以是完整环绕，不必再为了像素回归砍成只有横幅。
function settleEntrance() {
  if (!entranceShot) return;
  entranceShot = null;
  Object.assign(cam, tunedPreset(viewMode));
  tween = null;
  ceremonyPulse = null;
  camera.fov = cam.fov;
  camera.updateProjectionMatrix();
  if (entranceFx) {
    entranceFx.group.visible = false;
    entranceFx.line.material.opacity = 0;
    entranceFx.ring.material.opacity = 0;
    for (const entry of entranceFx.lights) entry.light.intensity = entry.base;
    if (bloomPass) bloomPass.strength = entranceFx.bloomBase;
  }
  updateCamera(performance.now());
}

function startEntranceOrbit(event, payload) {
  if (!state.board || !entranceFx) return false;
  settleEntrance();              // 连播时先把上一段收干净，避免两段镜头叠在一起
  const frames = makeEntranceFrames();
  const stations = frames.slice(1, 4);
  ceremonyPulse = null;
  if (bloomPass) bloomPass.strength = entranceFx.bloomBase;
  entranceFx.group.visible = true;
  entranceShot = {
    start: performance.now(),
    duration: event.duration_ms,
    frames,
    labels: stations.map((frame) => frame.label),
    subjects: stations.map((frame) => frame.subject),
    stage: -1,
    // 自动入场期间无头探针会 settle 后再取像素基线；手动重播 / ?event= 不打断。
    auto: !!payload.auto,
  };
  driveEntranceOrbit(0);
  updateCamera(performance.now());
  aimEntranceCamera();   // 第一帧就对准锚点，别等 animate
  return true;
}

function applyCeremonyFx(event, payload) {
  if (event.id === "entrance") {
    // 入场是完整环绕镜头：自动播放、控件重播、?event=entrance 一视同仁；
    // 像素回归的稳定性由 probe() 里的 settleEntrance() 负责，不再砍掉演出。
    startEntranceOrbit(event, payload);
    return;
  }
  const fx = CEREMONY_FX[event.id] ?? CEREMONY_FX.entrance;
  ceremonyPulse = {
    start: performance.now(), duration: event.duration_ms,
    fov: fx.fov, bloom: fx.bloom, dust: fx.dust,
    bloomBase: bloomPass?.strength ?? 0,
  };
}

function playCeremony(id, payload = {}) {
  if (!ceremony || !ceremonyDoc) return false;
  // 别的演出插进来时把入场镜头交还给用户，避免两段镜头 / 光效叠在一起。
  if (id !== "entrance" && entranceShot) cancelEntranceOrbit();
  // 演示用的回合 / 连胜 / 等级来自契约里的 sample_payload；真实对局由宿主
  // 直接传 payload 覆盖。棋盘自己不判定胜负、不累加连胜、不推断等级。
  const sample = ceremonyDoc.events.find((event) => event.id === id)?.sample_payload ?? {};
  return ceremony.dispatch(id, { ...sample, ...payload });
}

function bindEvents() {
  const canvas = renderer.domElement;
  // 拖过整个视口宽 ≈ 100°、整个视口高 ≈ 55°；按视口归一化，分辨率无关。
  const DRAG_YAW_SWEEP = 100 * DEG;
  const DRAG_PITCH_SWEEP = 55;

  // 移动端手势：CSS 的 touch-action / overscroll-behavior 之外再兜一层，
  // 防止 canvas 上的垂直拖动被当成页面滚动或下拉刷新。交互控件内不拦，
  // 否则原生 select / checkbox 会被吃掉。
  let pinchDistance = 0;
  canvas.addEventListener("touchstart", (event) => {
    if (event.touches.length >= 2) {
      dragging = null;
      pinchDistance = Math.hypot(
        event.touches[0].clientX - event.touches[1].clientX,
        event.touches[0].clientY - event.touches[1].clientY);
    }
  }, { passive: true });
  canvas.addEventListener("touchmove", (event) => {
    event.preventDefault();
    // 触屏没有滚轮，用双指捏合推进 / 拉远。
    if (event.touches.length >= 2 && pinchDistance > 0) {
      const distance = Math.hypot(
        event.touches[0].clientX - event.touches[1].clientX,
        event.touches[0].clientY - event.touches[1].clientY);
      if (distance > 0) {
        tween = null;
        cam.radius = THREE.MathUtils.clamp(
          cam.radius * (pinchDistance / distance), 12, 170);
        pinchDistance = distance;
      }
    }
  }, { passive: false });
  canvas.addEventListener("touchend", () => { pinchDistance = 0; }, { passive: true });
  document.addEventListener("touchmove", (event) => {
    const target = event.target;
    if (target instanceof Element
      && target.closest("#controls, #inspect, #thumb, #credits a")) return;
    event.preventDefault();
  }, { passive: false });
  // iOS Safari 双指缩放页面会毁掉全屏构图。
  document.addEventListener("gesturestart", (event) => event.preventDefault());

  canvas.addEventListener("pointerdown", (event) => {
    if (!event.isPrimary) return;
    cancelEntranceOrbit();
    dragging = {
      x: event.clientX, y: event.clientY,
      azimuth: cam.azimuth, elevation: cam.elevation, moved: 0,
    };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener("pointermove", (event) => {
    if (!dragging || !event.isPrimary) return;
    const dx = event.clientX - dragging.x;
    const dy = event.clientY - dragging.y;
    // dx/dy 是「相对按下点」的累积量。以前每个 pointermove 都拿完整 dx 去
    // 再减一次角度（cam.azimuth -= dx*0.004），等于把位移重复累加：轻轻拖
    // 40px、发 8 个 move 事件就会转出几十度。改成按下时角度的绝对映射。
    dragging.moved = Math.max(dragging.moved, Math.abs(dx) + Math.abs(dy));
    tween = null;
    const tuning = viewportTuning();
    cam.azimuth = dragging.azimuth - (dx / innerWidth) * DRAG_YAW_SWEEP * tuning.yaw;
    cam.elevation = THREE.MathUtils.clamp(
      dragging.elevation + (dy / innerHeight) * DRAG_PITCH_SWEEP * tuning.yaw,
      -14, 86);
  });
  canvas.addEventListener("pointerup", (event) => {
    // 手指比鼠标抖：触屏放宽点击阈值，否则点不中场景物件。
    if (dragging && dragging.moved < viewportTuning().tap) {
      pointer.set((event.clientX / innerWidth) * 2 - 1,
        -(event.clientY / innerHeight) * 2 + 1);
      raycaster.setFromCamera(pointer, camera);
      const hits = raycaster.intersectObjects(state.interactive, true);
      showInspect(hits.length ? hits[0].object : null);
      // 互动彩蛋：点中地面上的左下 / 右上棋手占位区（坐标来自 ZBRD anchors），
      // 用 y=0 平面求交而不是 mesh raycast，避免被树丛 / 屋舍挡住。
      if (!hits.length && state.board) {
        const point = new THREE.Vector3();
        if (raycaster.ray.intersectPlane(GROUND_PLANE, point)
          && state.board.anchors.some(([ax, az, r]) =>
            Math.hypot(point.x - ax, point.z - az) <= r * 1.4)) {
          playCeremony("easter_egg");
        }
      }
    }
    dragging = null;
  });
  // 浏览器接管手势（来电、系统手势、下拉刷新）会发 pointercancel，
  // 不清理 dragging 会让下一次拖拽从错误的角度起跳。
  canvas.addEventListener("pointercancel", () => { dragging = null; });
  canvas.addEventListener("wheel", (event) => {
    event.preventDefault();
    cancelEntranceOrbit();
    tween = null;
    cam.radius = THREE.MathUtils.clamp(cam.radius * (1 + event.deltaY * 0.0012), 12, 170);
  }, { passive: false });

  addEventListener("keydown", (event) => {
    if (event.key === "e" || event.key === "E") {
      playCeremony("easter_egg");
      return;
    }
    const step = 1.6;
    const forward = cam.azimuth;
    let dx = 0;
    let dz = 0;
    if (event.key === "ArrowUp") { dx = -Math.sin(forward) * step; dz = -Math.cos(forward) * step; }
    else if (event.key === "ArrowDown") { dx = Math.sin(forward) * step; dz = Math.cos(forward) * step; }
    else if (event.key === "ArrowLeft") { dx = -Math.cos(forward) * step; dz = Math.sin(forward) * step; }
    else if (event.key === "ArrowRight") { dx = Math.cos(forward) * step; dz = -Math.sin(forward) * step; }
    else return;
    event.preventDefault();
    cancelEntranceOrbit();
    tween = null;
    cam.panX = THREE.MathUtils.clamp(cam.panX + dx, -40, 40);
    cam.panZ = THREE.MathUtils.clamp(cam.panZ + dz, -40, 40);
  });

  $("view-immersive").addEventListener("click", () => setView("immersive"));
  $("view-overview").addEventListener("click", () => setView("overview"));
  // 切换世界重载整页：天空 / 地面贴图 / 物件都是按视觉包一次性生成的，
  // 重载比现场拆掉所有 InstancedMesh 更稳，URL 也能直接分享某一画境。
  $("world-select").addEventListener("change", (event) => {
    const url = new URL(location.href);
    url.searchParams.set("world", event.target.value);
    location.assign(url.toString());
  });
  $("toggle-bloom").addEventListener("change", (event) => {
    if (bloomPass) bloomPass.enabled = event.target.checked;
  });
  $("toggle-stars").addEventListener("change", (event) => {
    fxGroup.visible = event.target.checked;
  });
  $("toggle-tour").addEventListener("change", (event) => {
    tour = event.target.checked;
    if (tour) cancelEntranceOrbit();
  });
  $("inspect-close").addEventListener("click", () => showInspect(null));
  $("ceremony-select").addEventListener("change", (event) => {
    const id = event.target.value;
    event.target.value = "";
    if (id) playCeremony(id);
  });

  addEventListener("resize", () => {
    // 横竖屏档位变化会重设 fov / radius：先收掉入场镜头，避免两套构图互相覆盖。
    if (entranceShot) settleEntrance();
    camera.aspect = innerWidth / innerHeight;
    camera.updateProjectionMatrix();
    renderer.setSize(innerWidth, innerHeight);
    if (composer) composer.setSize(innerWidth, innerHeight);
    // 只在横竖屏档位变化时重设构图；地址栏收起这类普通 resize
    // 不能覆盖用户刚用滚轮 / 捏合调好的距离。
    const tuning = viewportTuning();
    if (tuning.key !== lastTuneKey) {
      lastTuneKey = tuning.key;
      const preset = tunedPreset(viewMode);
      cam.fov = preset.fov;
      cam.radius = preset.radius;
      tween = null;
    }
  });
}

function render() {
  if (composer) composer.render();
  else renderer.render(scene, camera);
}

function animate(time) {
  requestAnimationFrame(animate);
  if (dust) dust.rotation.y = time * 0.000018;
  if (skyMesh) skyMesh.rotation.y = (state.visual.skybox.rotation_deg ?? 0) * DEG
    + time * 0.000004;
  // 入场环绕临时接管 cam：自己插值半径 / 方位 / 仰角 / fov，结束时精确复位。
  // dust 基线先设好，入场脉冲是叠在它上面的附加转速。
  if (entranceShot) {
    updateEntranceShot(time);
  } else if (tween) {
    const t = THREE.MathUtils.clamp((time - tween.start) / tween.duration, 0, 1);
    const s = t * t * (3 - 2 * t);
    for (const key of Object.keys(tween.to)) {
      cam[key] = THREE.MathUtils.lerp(tween.from[key], tween.to[key], s);
    }
    if (t >= 1) tween = null;
  }
  if (tour && !tween && !entranceShot) cam.azimuth += 0.00012 * 16;
  updateCamera(time);
  // 入场每帧都对准场景锚点；结束后 entranceShot 清空，构图回到默认 lookAt。
  aimEntranceCamera();
  // 演出脉冲必须在 updateCamera 之后：它会临时覆盖 camera.fov，但从不写回
  // cam，所以拖拽 / setView / resize 的构图完全不受影响。entrance 不走这里。
  if (ceremonyPulse) {
    const pulse = ceremonyPulse;
    const t = THREE.MathUtils.clamp((time - pulse.start) / pulse.duration, 0, 1);
    const decay = 1 - t;
    const env = Math.sin(Math.PI * t);
    camera.fov = cam.fov + pulse.fov * decay;
    camera.updateProjectionMatrix();
    if (bloomPass) bloomPass.strength = pulse.bloomBase + pulse.bloom * env;
    if (dust) dust.rotation.y += pulse.dust * env;
    if (t >= 1) {
      camera.fov = cam.fov;
      camera.updateProjectionMatrix();
      if (bloomPass) bloomPass.strength = pulse.bloomBase;
      ceremonyPulse = null;
    }
  }
  render();
}

// 无头验证钩子：同一任务内 render + readPixels（前缓冲默认不保留，跨任务读
// 只会得到被清空的黑帧）。capture() 直接把帧缓冲翻正后编码成 PNG data URL。
window.__zengineStarry = {
  scene, camera, renderer, boardGroup, scenePropsGroup, islandGroup, fxGroup, state,
  get composer() { return composer; },
  get bloomPass() { return bloomPass; },
  view() { return { ...cam }; },
  setView, render,
  // 宿主（游戏逻辑 / 控制台 / 自动化）从这里派发演出；棋盘不判定胜负。
  ceremony: {
    dispatch: (id, payload) => playCeremony(id, payload),
    last: () => ceremony?.last() ?? null,
    active: () => ceremony?.active() ?? null,
    history: () => ceremony?.history() ?? [],
    eventIds: () => ceremony?.eventIds() ?? [],
    stop: () => {
      if (entranceShot) settleEntrance();
      ceremony?.stop();
    },
    orbitActive: () => !!entranceShot,
    look: () => entranceShot?.look ?? null,
    settle: settleEntrance,
  },
  probe() {
    // 自动入场期间无头探针要取像素基线：先把环绕镜头锁定回默认构图，
    // 保证 topSky / center / ground / left / right 采样永远落在同一帧。
    if (entranceShot?.auto) settleEntrance();
    // 直出一次只取 draw call / 三角形统计（composer 的 info 只反映最后一个 pass）。
    renderer.render(scene, camera);
    const drawCalls = renderer.info.render.calls;
    const triangles = renderer.info.render.triangles;
    render();
    const gl = renderer.getContext();
    const width = gl.drawingBufferWidth;
    const height = gl.drawingBufferHeight;
    const pick = (fx, fy) => {
      const pixel = new Uint8Array(4);
      gl.readPixels(Math.floor(width * fx), Math.floor(height * fy), 1, 1,
        gl.RGBA, gl.UNSIGNED_BYTE, pixel);
      return [...pixel.slice(0, 3)];
    };
    let islandMeshes = 0;
    islandGroup.traverse((object) => { if (object.isMesh) islandMeshes += 1; });
    let boardMeshes = 0;
    boardGroup.traverse((object) => { if (object.isMesh) boardMeshes += 1; });
    let sceneProps = 0;
    scenePropsGroup.traverse((object) => { if (object.isMesh) sceneProps += 1; });
    return {
      useExternalProps: USE_EXTERNAL_PROPS,
      world: state.pack?.id ?? null,
      worldTier: state.pack?.tier ?? null,
      worldCount: state.worlds?.worlds.length ?? 0,
      sceneryPreset: state.skin?.scenery.preset ?? null,
      skyMode: state.pack?.sky.mode ?? null,
      conceptStatus: state.pack?.concept.status ?? null,
      artwork: state.pack?.artwork ? {
        mode: state.pack.artwork.mode,
        image: state.pack.artwork.image,
        visible: !$("artwork").hidden,
        boardVisible: !$("artwork-grid").hidden,
      } : null,
      boardLayer: state.artBoard ? {
        columns: state.artBoard.columns,
        rows: state.artBoard.rows,
        anchors: state.artBoard.anchors,
        pieces: state.artBoard.pieces.size,
      } : null,
      sceneProps,
      canvas: [width, height],
      topSky: pick(0.5, 0.9), horizon: pick(0.5, 0.58),
      center: pick(0.5, 0.5), ground: pick(0.5, 0.16),
      left: pick(0.2, 0.45), right: pick(0.8, 0.45),
      boardOutline: state.board?.outline.length ?? 0,
      boardAnchors: state.board?.anchors.length ?? 0,
      boardHalf: state.board?.half.map((v) => +v.toFixed(2)) ?? null,
      // 棋盘不再有实体台面：boardGroup 里只剩区域外沿的外部装饰物。
      boardSlabMeshes: boardGroup.children.filter((object) => !object.userData?.external).length,
      boardMeshes, islandMeshes,
      external: { ...state.external },
      skyTexture: skyMesh?.material?.map?.image
        ? [skyMesh.material.map.image.width, skyMesh.material.map.image.height] : null,
      skyColorSpace: skyMesh?.material?.map?.colorSpace ?? null,
      bloom: bloomPass ? { enabled: bloomPass.enabled, strength: bloomPass.strength } : null,
      fog: scene.fog ? { density: scene.fog.density } : null,
      drawCalls, triangles,
      camera: camera.position.toArray().map((v) => +v.toFixed(1)),
      ceremony: ceremony ? {
        events: ceremony.eventIds().length,
        last: ceremony.last()?.id ?? null,
        active: ceremony.active(),
        history: ceremony.history().length,
      } : null,
    };
  },
  capture() {
    render();
    const gl = renderer.getContext();
    const width = gl.drawingBufferWidth;
    const height = gl.drawingBufferHeight;
    const pixels = new Uint8Array(width * height * 4);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(width, height);
    for (let y = 0; y < height; y += 1) {
      const src = (height - 1 - y) * width * 4;
      image.data.set(pixels.subarray(src, src + width * 4), y * width * 4);
    }
    ctx.putImageData(image, 0, 0);
    return canvas.toDataURL("image/png");
  },
};

// 原画直出（第 13 画境）：把实机截图按原比例整幅铺在 3D 画布之上。
// 这是唯一能保证与来源图逐像素一致的做法 —— 程序化 3D 几何或 AI 生成的单件
// 模型都只能做语义近似。3D 场景照常构建、只是 opacity 归零，因此 13 画境的
// 地面签名 / boardSlabMeshes / ceremony / 入场分镜契约一个都不破。
function applyArtwork(pack) {
  const art = pack.artwork;
  if (!art || art.mode !== "pixel_faithful") return;
  const root = $("artwork");
  const img = $("artwork-img");
  const src = `../${art.image}`;
  img.src = src;
  img.alt = `${pack.title_zh} · 实机棋盘截图`;
  root.style.setProperty("--artwork-blur", `url("${src}")`);
  root.style.backgroundColor = art.backdrop ?? "#2a1a24";
  root.hidden = false;
  document.body.classList.add("artwork-mode");
  // ?clean=1 连 HUD 一起隐藏，得到与来源截图逐像素一致的画面。
  if (new URLSearchParams(location.search).has("clean")) {
    document.body.classList.add("clean");
  }
  $("artwork-controls").hidden = false;
  $("thumb").title = `${pack.title_zh} · 实机截图（原画直出）`;
  $("hint").textContent = "原画直出 · 右侧可切换 3D 近似，或打开可落子的真实棋盘";

  // 真正的棋盘层：行列数 / 逻辑边界 / 两个棋手锚点全部来自
  // build/board.manifest.json（loadBoardRegion 已写进 state.manifest），
  // art.grid.quad 只负责把世界坐标透视映射到原图像素坐标。
  // 冻结契约只要求「最外一个方格 + 两个棋手占位」；7×8 是逻辑落点、不是可见几何，
  // 所以这一层是**可开关的交互工具层**，默认关闭以保持原画 1:1。
  const canvas = $("artwork-grid");
  canvas.width = art.size_px[0];
  canvas.height = art.size_px[1];
  const ctx = canvas.getContext("2d");
  const spec = state.manifest.grid;
  const cols = spec.columns;               // 7
  const rows = spec.rows;                   // 8
  const bounds = spec.logical_bounds_xz;    // [-8.062, 8.063, -7.2, 7.2]
  const anchors = state.manifest.anchors ?? [];
  const q = art.grid.quad;

  const at = (u, v) => {
    const top = [q[0][0] + (q[1][0] - q[0][0]) * u,
      q[0][1] + (q[1][1] - q[0][1]) * u];
    const bottom = [q[3][0] + (q[2][0] - q[3][0]) * u,
      q[3][1] + (q[2][1] - q[3][1]) * u];
    return [top[0] + (bottom[0] - top[0]) * v,
      top[1] + (bottom[1] - top[1]) * v];
  };
  // 双线性四边形的逆映射（牛顿迭代）：把鼠标像素还原成 (u,v)。
  const invert = (px, py) => {
    let u = .5, v = .5;
    for (let i = 0; i < 20; i += 1) {
      const p = at(u, v), pu = at(u + 1e-3, v), pv = at(u, v + 1e-3);
      const a = (pu[0] - p[0]) * 1e3, b = (pv[0] - p[0]) * 1e3;
      const c = (pu[1] - p[1]) * 1e3, d = (pv[1] - p[1]) * 1e3;
      const det = a * d - b * c;
      if (Math.abs(det) < 1e-9) break;
      const ex = px - p[0], ey = py - p[1];
      u += (d * ex - b * ey) / det;
      v += (a * ey - c * ex) / det;
    }
    return [u, v];
  };
  const worldToUV = (x, z) => [
    (x - bounds[0]) / (bounds[1] - bounds[0]),
    (z - bounds[2]) / (bounds[3] - bounds[2]),
  ];
  const cellQuad = (col, row) => [
    at(col / cols, row / rows), at((col + 1) / cols, row / rows),
    at((col + 1) / cols, (row + 1) / rows), at(col / cols, (row + 1) / rows),
  ];
  const fillQuad = (points, color) => {
    ctx.beginPath();
    points.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.fill();
  };
  const stroke = (points, alpha, width) => {
    ctx.beginPath();
    points.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
    ctx.strokeStyle = `rgba(255, 242, 208, ${alpha})`;
    ctx.lineWidth = width;
    ctx.stroke();
  };

  const pieces = new Map();   // "col,row" -> 0/1，交替落子
  let hover = null;
  let side = 0;
  state.artBoard = { columns: cols, rows, anchors: anchors.length, pieces };

  const draw = () => {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    // 冻结核心：最外一个方格（一整块连续台面，不是 56 个独立格子块）。
    fillQuad([at(0, 0), at(1, 0), at(1, 1), at(0, 1)],
      "rgba(255, 238, 205, .045)");
    stroke([at(0, 0), at(1, 0), at(1, 1), at(0, 1), at(0, 0)], .85, 3);
    // 7×8 逻辑落点：只画参考线，不产生任何几何。
    const steps = [0, .25, .5, .75, 1];
    for (let c = 1; c < cols; c += 1) {
      stroke(steps.map((v) => at(c / cols, v)), .34, 1.6);
    }
    for (let r = 1; r < rows; r += 1) {
      stroke(steps.map((u) => at(u, r / rows)), .34, 1.6);
    }
    if (hover) fillQuad(cellQuad(hover[0], hover[1]), "rgba(255, 214, 128, .28)");
    for (const [key, player] of pieces) {
      const [col, row] = key.split(",").map(Number);
      const quad = cellQuad(col, row);
      const cx = quad.reduce((s, p) => s + p[0], 0) / 4;
      const cy = quad.reduce((s, p) => s + p[1], 0) / 4;
      ctx.beginPath();
      ctx.arc(cx, cy, 15, 0, Math.PI * 2);
      ctx.fillStyle = player ? "rgba(178, 132, 232, .88)" : "rgba(255, 198, 92, .92)";
      ctx.shadowColor = player ? "#c9a6f2" : "#ffd27a";
      ctx.shadowBlur = 18;
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.lineWidth = 2;
      ctx.strokeStyle = "rgba(255, 248, 226, .8)";
      ctx.stroke();
    }
    // 冻结核心：左下 / 右上两个棋手占位，坐标来自 anchor_* 部件。
    ctx.font = "18px system-ui, sans-serif";
    ctx.textAlign = "center";
    for (let i = 0; i < anchors.length; i += 1) {
      const [x, , z] = anchors[i].position;
      const [u, v] = worldToUV(x, z);
      const [px, py] = at(u, v);
      ctx.beginPath();
      ctx.ellipse(px, py, 30, 18, 0, 0, Math.PI * 2);
      ctx.strokeStyle = "rgba(255, 230, 180, .9)";
      ctx.lineWidth = 2.5;
      ctx.stroke();
      ctx.fillStyle = "rgba(46, 22, 32, .62)";
      ctx.fillRect(px - 46, py - 13, 92, 26);
      ctx.fillStyle = "#ffe9c0";
      ctx.fillText(`棋手 0${i + 1}`, px, py + 6);
    }
  };

  const readout = $("board-readout");
  const cellAt = (event) => {
    const rect = canvas.getBoundingClientRect();
    const px = (event.clientX - rect.left) / rect.width * canvas.width;
    const py = (event.clientY - rect.top) / rect.height * canvas.height;
    const [u, v] = invert(px, py);
    if (u < 0 || u > 1 || v < 0 || v > 1) return null;
    return [Math.min(cols - 1, Math.floor(u * cols)),
      Math.min(rows - 1, Math.floor(v * rows))];
  };
  canvas.addEventListener("pointermove", (event) => {
    hover = cellAt(event);
    if (hover) {
      const x = bounds[0] + (bounds[1] - bounds[0]) * ((hover[0] + .5) / cols);
      const z = bounds[2] + (bounds[3] - bounds[2]) * ((hover[1] + .5) / rows);
      readout.textContent = `格 ${hover[0] + 1}/${cols} · ${hover[1] + 1}/${rows}`
        + `　世界 (${x.toFixed(2)}, ${z.toFixed(2)})`;
    } else {
      readout.textContent = "棋盘外";
    }
    draw();
  });
  canvas.addEventListener("pointerleave", () => {
    hover = null; readout.textContent = ""; draw();
  });
  canvas.addEventListener("click", (event) => {
    const cell = cellAt(event);
    if (!cell) return;
    const key = cell.join(",");
    if (pieces.has(key)) pieces.delete(key);
    else pieces.set(key, side++ % 2);
    draw();
  });

  const enableBoard = (on) => {
    canvas.hidden = !on;
    readout.hidden = !on;
    document.body.classList.toggle("board-mode", on);
    if (on) draw();
  };
  const toggleBoard = $("toggle-board");
  toggleBoard.addEventListener("change", (event) => enableBoard(event.target.checked));
  // ?board=1 直接以可落子棋盘打开；?clean=1 仍是纯原画（两者同时给时纯图优先）。
  const params = new URLSearchParams(location.search);
  const boardParam = params.has("board") && !params.has("clean");
  toggleBoard.checked = boardParam;
  enableBoard(boardParam);

  $("toggle-artwork").addEventListener("change", (event) => {
    document.body.classList.toggle("artwork-mode", event.target.checked);
    root.hidden = !event.target.checked;
  });
}

async function main() {
  if (location.protocol === "file:") {
    fail("请通过专用静态服务打开：python integration/chessboard/tools/serve.py\n\n"
      + "然后访问 http://127.0.0.1:8080/web/starry.html");
    return;
  }
  try {
    const [baseVisual, spec, worldsDoc, ceremonyDocument] = await Promise.all([
      fetchJSON("../visual-pack.json"),
      fetchJSON("../board-spec.json"),
      fetchJSON("./worlds.json"),
      fetchJSON("../ceremony.json"),
    ]);
    if (worldsDoc.schema !== WORLD_PACK_SCHEMA) {
      throw new Error(`worlds.json schema 不匹配：${worldsDoc.schema}`);
    }
    const pack = resolveWorldPack(worldsDoc, selectedWorldId(worldsDoc));
    state.worlds = worldsDoc;
    state.pack = pack;
    state.skin = {
      sky: pack.sky, ground: pack.ground, board: pack.board,
      scenery: pack.scenery, lighting: pack.lighting,
    };
    const visual = worldVisual(baseVisual, pack);
    state.visual = visual;
    state.spec = spec;
    applyWorldHud(worldsDoc, pack, $);
    ceremonyDoc = ceremonyDocument;
    ceremony = createCeremony({
      doc: ceremonyDocument,
      root: $("ceremony"),
      palette: pack.scenery.ceremony ?? {},
      onEvent: applyCeremonyFx,
    });

    // 先解析 ZBRD 拿到棋盘区域轮廓，再把它画进地形贴图；顺序不能反。
    // 原画棋盘层也要等 state.manifest 就绪：行列 / 逻辑边界 / 锚点都从它读。
    state.board = await loadBoardRegion();
    applyArtwork(pack);
    // 星夜用 sha 锁定的同源全景图；另外 11 个世界按视觉包调色板程序化画天穹。
    state.skyTexture = pack.sky.mode === "panorama_sha_locked"
      ? await loadShaTexture(
        visual.skybox.panorama_image, visual.skybox.panorama_sha256)
      : makeSkyPanorama(THREE, pack);
    buildSky(visual);
    buildGround(state.board);
    if (pack.scenery.preset === "starry" || pack.scenery.preset === "generic") {
      registerStarryFeatures(buildFoliage());
    } else {
      buildWorldScenery(pack.scenery.preset, {
        group: islandGroup, propsGroup: scenePropsGroup, skin: state.skin,
        random: mulberry32(90210 + pack.index * 977),
        CLEAR_R, ISLAND_R, PATHS, interactive: state.interactive,
        features: state.features,
      });
    }
    buildSceneProps();
    buildParticles(spec);
    addLights(visual);
    await setupPost(visual);
    // 默认只有场景物件；改 USE_EXTERNAL_PROPS 才把 6 个混元单体陈列出来验收。
    if (USE_EXTERNAL_PROPS) {
      await loadExternalMeshes(visual);
    }
    state.interactive = state.interactive.filter((item) => item.visible);
    buildEntranceFx();
    bindEvents();
    animate(performance.now());
    $("loading").classList.add("done");
    // 加载完成自动播完整环绕入场；?event=<id> 可直接分享 / 回归任一演出。
    const requestedEvent = new URLSearchParams(location.search).get("event");
    if (!requestedEvent || !playCeremony(requestedEvent)) {
      playCeremony("entrance", { auto: true });
    }
  } catch (error) {
    fail(`${state.pack?.title_zh ?? "画境"}场景加载失败：${error.message}\n\n`
      + "请确认已执行 python integration/chessboard/zboard.py --variants，"
      + "并通过 tools/serve.py 打开页面。");
    console.error("[ZEngine] starry page failed", error);
  }
}

main();
