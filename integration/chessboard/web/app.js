// three.js 宣发页：只读 ZBRD，不再生成几何。
// 几何变体 / 画质档切换都是消费同一份产物，保证与 Unity HD 场景里的棋盘一致。
// 核心区是一整块连续台面（7x8 只是逻辑落点），外围装饰由 style-pack.json
// 决定；场景视觉层（天空 / 后处理 / 材质 / Volume 参数）由 AI 生成的
// visual-pack.json 决定。本页同时展示「冻结核心 / 可变风格 / AI 视觉」的拆分。
// 参考 painting3d.rockeymen.xyz/starry-night 的架构：
// WebGLRenderer + 自定义 ShaderMaterial 天穹 + Points 星空 + 拖拽环视。

import * as THREE from "three";
import { parseZBRD, filterByLod, KIND, LOD_BITS } from "./zbrd.mjs";
import {
  EXTERNAL_MANIFEST_SCHEMA, applyTransform, groupPlacements,
  readGlbHeader, verifySha256,
} from "./external_mesh.mjs";

const BUILD = "../build/";
const $ = (id) => document.getElementById(id);

// 外部单体装饰（glb + sha256 + 锚点）：与 Unity / Blender 消费同一份清单。
const externalObjects = [];
const externalSummary = {
  applied: 0, total: 0, textured: 0, error: "", textureError: "",
};

const state = {
  spec: null,
  manifest: null,
  variants: null,
  stylePack: null,
  visualPack: null,
  parts: [],
  lod: "HD",
  variantFile: "board.zbrd",
  shape: null,
  selected: null,
  interactive: [],
};

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(48, innerWidth / innerHeight, 0.1, 400);
const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.outputColorSpace = THREE.SRGBColorSpace;
$("stage").appendChild(renderer.domElement);

const boardGroup = new THREE.Group();
scene.add(boardGroup);

let yaw = 0.62;
let pitch = 0.58;
let distance = 36;

function updateCamera() {
  const cp = Math.cos(pitch);
  camera.position.set(Math.sin(yaw) * cp * distance,
                      Math.sin(pitch) * distance,
                      Math.cos(yaw) * cp * distance);
  camera.lookAt(0, 0, 0);
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

async function fetchBoard(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path} -> HTTP ${response.status}`);
  return parseZBRD(await response.arrayBuffer());
}

// ZBRD / board-spec / style-pack / visual-pack 里的颜色都是 sRGB 设计值
//（见 blender_export.py 的 srgb_to_linear 注释），three.js 在线性空间工作，
// 所以这里必须把 sRGB 解码成线性。不要去掉 SRGBColorSpace——那会把线性值当
// sRGB 再压暗一次，或者反过来让颜色发白。
function srgb(rgba) {
  return new THREE.Color().setRGB(rgba[0], rgba[1], rgba[2], THREE.SRGBColorSpace);
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

const SKY_VERT = /* glsl */`
varying vec3 vDir;
void main() {
  vDir = position;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;

const SKY_FRAG = /* glsl */`
uniform vec3 uTop;
uniform vec3 uHorizon;
uniform vec3 uBottom;
varying vec3 vDir;
void main() {
  float h = normalize(vDir).y;
  vec3 c = h > 0.0
    ? mix(uHorizon, uTop, pow(h, 0.62))
    : mix(uHorizon, uBottom, pow(-h, 0.48));
  gl_FragColor = vec4(c, 1.0);
  #include <colorspace_fragment>
}`;

function makeGeometry(part) {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(part.positions, 3));
  geometry.setAttribute("normal", new THREE.BufferAttribute(part.normals, 3));
  geometry.setAttribute("uv", new THREE.BufferAttribute(part.uvs, 2));
  geometry.setIndex(new THREE.BufferAttribute(part.indices, 1));
  geometry.computeBoundingSphere();
  return geometry;
}

function disposeGroup() {
  boardGroup.traverse((object) => {
    if (!object.isMesh && !object.isPoints) return;
    // 外部资产与缓存共享 geometry / material，不能随 ZBRD 部件一起释放。
    if (object.userData?.external) return;
    object.geometry?.dispose();
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    for (const material of materials) material?.dispose();
  });
  boardGroup.clear();
  state.interactive = [];
}

// JSON 里 Directional/Ambient 的强度按 Unity / 旧版 three.js 语义调好（1.0≈中性）；
// r155+ 物理灯光模式按 lux 解释，同一数值会暗 π 倍，消费时 ×π 对齐
//（不要用已废弃的 renderer.useLegacyLights）。
const LUX_SCALE = Math.PI;

function addLights() {
  // AI 视觉包只覆写颜色 / 强度，几何与灯光欧拉角仍来自冻结 spec。
  const base = state.spec.lighting;
  const light = state.visualPack
    ? { ...base, ...state.visualPack.lighting } : base;
  const directional = new THREE.DirectionalLight(
    srgb(light.directional_color), light.directional_intensity * LUX_SCALE);
  const e = light.directional_euler;
  directional.position.set(
    Math.sin(e[1] * Math.PI / 180) * 30,
    Math.max(6, Math.cos(e[0] * Math.PI / 180) * 40),
    Math.cos(e[1] * Math.PI / 180) * 30);
  scene.add(directional);
  scene.add(new THREE.AmbientLight(
    srgb(light.ambient_color), light.ambient_intensity * LUX_SCALE));
}

function addStars() {
  const sky = state.spec.sky;
  const scale = state.manifest.lods[state.lod].star_scale;
  const count = Math.max(0, Math.floor(sky.stars * scale));
  if (!count) return null;
  const random = mulberry32(sky.star_seed);
  const radius = sky.radius_m * 0.9;
  const positions = new Float32Array(count * 3);
  for (let i = 0; i < count; i += 1) {
    const u = random() * 2 - 1;
    const theta = random() * Math.PI * 2;
    const r = Math.sqrt(1 - u * u);
    positions[i * 3] = radius * r * Math.cos(theta);
    positions[i * 3 + 1] = Math.abs(radius * u) * 0.85 + 1.0;
    positions[i * 3 + 2] = radius * r * Math.sin(theta);
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  const material = new THREE.PointsMaterial({
    color: 0xe9e2cf, size: 1.7, sizeAttenuation: false,
    transparent: true, opacity: 0.78, depthWrite: false,
  });
  const points = new THREE.Points(geometry, material);
  boardGroup.add(points);
  return count;
}

// 轻量 glb 几何解析：只读 POSITION/NORMAL/TEXCOORD_0/indices + 首节点变换，
// 不碰贴图。混元 glb 内嵌贴图会让 GLTFLoader 的 ImageBitmapLoader 解码挂起
// （Promise 不 settle），而本页本来就用代理材质（map=null）、贴图毫无用处，
// 所以绕开 GLTFLoader 的贴图加载，直接按 accessor 读几何。
const GLB_MAGIC = 0x46546c67;
const GLB_CHUNK_JSON = 0x4e4f534a;
const GLB_CHUNK_BIN = 0x004e4942;
const GLB_COMPONENT = { 5121: Uint8Array, 5122: Int16Array, 5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array };
const GLB_TYPE_SIZE = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };

function parseExternalGlb(bytes) {
  const view = new DataView(bytes);
  if (view.getUint32(0, true) !== GLB_MAGIC) throw new Error("bad glb magic");
  const jsonLen = view.getUint32(12, true);
  if (view.getUint32(16, true) !== GLB_CHUNK_JSON) throw new Error("bad JSON chunk");
  const doc = JSON.parse(new TextDecoder().decode(new Uint8Array(bytes, 20, jsonLen)));
  const binAt = 20 + jsonLen;
  const binLen = view.getUint32(binAt, true);
  if (view.getUint32(binAt + 4, true) !== GLB_CHUNK_BIN) throw new Error("bad BIN chunk");
  const bin = new Uint8Array(bytes, binAt + 8, binLen);

  const readAccessor = (index) => {
    const accessor = doc.accessors[index];
    const bufferView = doc.bufferViews[accessor.bufferView];
    const TypedArray = GLB_COMPONENT[accessor.componentType];
    const dim = GLB_TYPE_SIZE[accessor.type];
    const bytesPerElement = TypedArray.BYTES_PER_ELEMENT;
    const stride = bufferView.byteStride || (bytesPerElement * dim);
    const base = bufferView.byteOffset + (accessor.byteOffset || 0);
    const count = accessor.count;
    if (stride === bytesPerElement * dim && base % bytesPerElement === 0) {
      return new TypedArray(bin.buffer, bin.byteOffset + base, count * dim);
    }
    const out = new TypedArray(count * dim);
    for (let i = 0; i < count; i++) {
      const offset = base + i * stride;
      for (let j = 0; j < dim; j++) {
        out[i * dim + j] = new TypedArray(bin.buffer, bin.byteOffset + offset + j * bytesPerElement, 1)[0];
      }
    }
    return out;
  };

  const primitive = doc.meshes[0].primitives[0];
  const geometry = new THREE.BufferGeometry();
  if (primitive.attributes.POSITION !== undefined) {
    geometry.setAttribute("position", new THREE.BufferAttribute(readAccessor(primitive.attributes.POSITION), 3));
  }
  if (primitive.attributes.NORMAL !== undefined) {
    geometry.setAttribute("normal", new THREE.BufferAttribute(readAccessor(primitive.attributes.NORMAL), 3));
  }
  if (primitive.attributes.TEXCOORD_0 !== undefined) {
    geometry.setAttribute("uv", new THREE.BufferAttribute(readAccessor(primitive.attributes.TEXCOORD_0), 2));
  }
  if (primitive.indices !== undefined) {
    geometry.setIndex(new THREE.BufferAttribute(readAccessor(primitive.indices), 1));
  }
  geometry.computeBoundingSphere();

  // glb 的节点变换（混元产物恒为 rotation=[+90°X]，Z-up → Y-up）必须烘进几何：
  // 消费端 applyTransform 会重设 object 的 position / rotation / scale，若把变换
  // 留在节点 transform 上会被整体覆盖，资产躺倒 90°、高度只剩期望的 40~67%。
  // 与 external_mesh.py 的 glb_triangles（顶点 + 法线都过 _apply_point /
  // _normal_matrix）以及 Unity 的 ZBRD 缓存保持同一口径。
  const node = doc.nodes && doc.nodes[0];
  if (node) {
    const matrix = new THREE.Matrix4();
    if (node.matrix) {
      matrix.fromArray(node.matrix);
    } else {
      matrix.compose(
        new THREE.Vector3().fromArray(node.translation ?? [0, 0, 0]),
        new THREE.Quaternion().fromArray(node.rotation ?? [0, 0, 0, 1]),
        new THREE.Vector3().fromArray(node.scale ?? [1, 1, 1]));
    }
    geometry.applyMatrix4(matrix);
  }
  return new THREE.Mesh(geometry, new THREE.MeshStandardMaterial());
}

async function loadExternalMeshes() {
  externalObjects.length = 0;
  externalSummary.applied = 0;
  externalSummary.total = 0;
  externalSummary.textured = 0;
  externalSummary.error = "";
  externalSummary.textureError = "";
  let manifest;
  try {
    manifest = await fetchJSON(BUILD + "external_mesh.manifest.json");
  } catch {
    return; // 没有清单就保持 ZBRD 代理几何
  }
  if (!manifest || manifest.schema !== EXTERNAL_MANIFEST_SCHEMA) {
    externalSummary.error = "清单 schema 不匹配";
    return;
  }
  externalSummary.total = (manifest.placements ?? []).length;
  const { assets, groups } = groupPlacements(manifest);
  for (const [assetId, placements] of groups) {
    const asset = assets.get(assetId);
    try {
      // glb 是真源：先逐字节校验 sha256，再解析几何（不加载贴图）。
      const response = await fetch("../" + asset.mesh);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const bytes = await response.arrayBuffer();
      if (!(await verifySha256(bytes, asset.mesh_sha256))) {
        throw new Error("glb sha256 校验失败");
      }
      readGlbHeader(bytes);
      const base = parseExternalGlb(bytes);
      // baseColor 贴图走独立 PNG（三端共用同一份字节），不走 glb 内嵌贴图：
      // GLTFLoader 的 ImageBitmapLoader 解码在无头/受限环境会挂起，而自写的
      // parseExternalGlb 只解析几何。加载失败时降级为代理纯色，几何照常显示。
      let texture = null;
      if (asset.texture && asset.texture_sha256) {
        try {
          const texResponse = await fetch("../" + asset.texture);
          if (!texResponse.ok) throw new Error(`HTTP ${texResponse.status}`);
          const texBytes = await texResponse.arrayBuffer();
          if (!(await verifySha256(texBytes, asset.texture_sha256))) {
            throw new Error("texture sha256 校验失败");
          }
          const objectUrl = URL.createObjectURL(
            new Blob([texBytes], { type: "image/png" }));
          try {
            texture = await new THREE.TextureLoader().loadAsync(objectUrl);
          } finally {
            URL.revokeObjectURL(objectUrl);
          }
          texture.colorSpace = THREE.SRGBColorSpace;
          // glTF 的 TEXCOORD_0 是左上原点（V 向下），与 three.js 默认相反。
          texture.flipY = false;
          texture.needsUpdate = true;
          externalSummary.textured += 1;
        } catch (error) {
          texture = null;
          externalSummary.textureError = `${assetId}: ${error.message}`;
          console.warn("[ZEngine] baseColor texture skipped", assetId, error);
        }
      }
      for (const placement of placements) {
        const proxy = boardGroup.getObjectByName(placement.proxy_part);
        const object = base.clone(true);
        applyTransform(object, placement, asset.pivot_offset);
        object.name = `ext_${placement.instance_id}`;
        object.userData = {
          id: placement.instance_id,
          tag: placement.tag,
          kind: KIND.INTERACTABLE,
          external: true,
          proxyPart: placement.proxy_part,
        };
        // 三端统一按 glTF 语义：baseColor = map × baseColorFactor，factor 恒为白。
        // 代理纯色只在贴图缺失时作为降级，否则会把贴图颜色再压暗一层。
        object.traverse((child) => {
          if (!child.isMesh) return;
          child.material = child.material.clone();
          child.material.map = texture;
          child.material.needsUpdate = true;
          if (!proxy?.material) return;
          const source = proxy.material;
          if (texture) child.material.color.setRGB(1, 1, 1);
          else child.material.color.copy(source.color);
          child.material.roughness = source.roughness;
          child.material.metalness = source.metalness;
          child.material.emissive.copy(source.emissive);
          child.material.emissiveIntensity = source.emissiveIntensity;
        });
        externalObjects.push(object);
        externalSummary.applied += 1;
      }
    } catch (error) {
      externalSummary.error = `${assetId}: ${error.message}`;
      console.warn("[ZEngine] external mesh skipped", assetId, error);
    }
  }
}

function rebuild() {
  disposeGroup();
  const sky = state.spec.sky;
  const skybox = state.visualPack?.skybox;
  const skyColor = (key) => srgb(skybox?.[key] ?? sky[key]);
  const selected = filterByLod(state.parts, state.lod);

  for (const part of selected) {
    const geometry = makeGeometry(part);
    if (part.kind === KIND.SKY) {
      const material = new THREE.ShaderMaterial({
        vertexShader: SKY_VERT,
        fragmentShader: SKY_FRAG,
        side: THREE.DoubleSide,
        depthWrite: false,
        uniforms: {
          uTop: { value: skyColor("top_color") },
          uHorizon: { value: skyColor("horizon_color") },
          uBottom: { value: skyColor("bottom_color") },
        },
      });
      boardGroup.add(new THREE.Mesh(geometry, material));
      continue;
    }
    const interactive = part.kind === KIND.INTERACTABLE;
    const aiColor = state.visualPack?.materials?.[part.material]?.color;
    const material = new THREE.MeshStandardMaterial({
      color: srgb(aiColor ?? part.color),
      roughness: interactive ? 0.35 : 0.78,
      metalness: interactive ? 0.55 : 0.12,
      side: THREE.DoubleSide,
      emissive: interactive ? new THREE.Color(0x3a2a0c) : new THREE.Color(0x000000),
      emissiveIntensity: interactive ? 0.9 : 0.0,
    });
    const mesh = new THREE.Mesh(geometry, material);
    mesh.name = part.name;
    mesh.userData = { id: part.name, tag: part.tag, kind: part.kind };
    boardGroup.add(mesh);
    if (interactive) state.interactive.push(mesh);
  }

  // 外部单体资产与锚点无关 LOD / 变体，重新挂回并关掉同名代理。
  for (const object of externalObjects) {
    boardGroup.add(object);
    const proxy = boardGroup.getObjectByName(object.userData.proxyPart);
    if (proxy) proxy.visible = false;
    if (!object.userData.tag) continue;
    object.traverse((child) => {
      if (!child.isMesh) return;
      child.userData = object.userData;
      state.interactive.push(child);
    });
  }
  state.interactive = state.interactive.filter((item) => item.visible);

  const stars = addStars();
  const grid = state.manifest.grid;
  const coreCount = selected.filter((p) => p.name !== "cell_marks"
    && (p.name === "core_slab" || p.name.startsWith("frame_")
      || p.name.startsWith("anchor_") || p.name.startsWith("sky@"))).length;
  const variableCount = selected.length - coreCount;
  $("stat").textContent = [
    `${grid.columns} × ${grid.rows} 逻辑落点 · ${grid.layout}`,
    `核心：连续台面（${grid.visual_kind}），不是 56 个可见格子`,
    `轮廓 ${state.shape ?? grid.shape} / 刻线 ${grid.cell_marks}`,
    `风格包 ${state.stylePack.name} · ${state.manifest.variable_parts.length} 个外围装饰`,
    `AI 视觉层 ${state.visualPack
      ? `${state.visualPack.name} / ${state.visualPack.skybox.mode} 天空盒`
      : "未加载"}`,
    `外部单体 ${externalSummary.applied}/${externalSummary.total} 个实例`
      + `（glb + sha256 + 锚点 · ${externalSummary.textured} 张 baseColor 贴图，与 Unity / Blender 同源）`
      + (externalSummary.error ? ` · ${externalSummary.error}` : "")
      + (externalSummary.textureError ? ` · 贴图降级: ${externalSummary.textureError}` : ""),
    `当前档 ${selected.length} 个部件 = ${coreCount} 冻结核心 + ${variableCount} 风格化`
      + `${stars === null ? "" : ` · ${stars} 颗星`}`,
  ].join("\n");
  $("stat").style.whiteSpace = "pre-line";
  $("lod-note").textContent =
    `交付只产一个 HD 场景（Assets/Scenes/pvp/*_HD.unity），配置表不动；`
    + "High/Mid/Low 由项目自动降配工具从 HD 派生后美术手调。"
    + ` 这里的档位切换是宣发预览：lod_mask = ${LOD_BITS[state.lod]}，`
    + "Unity 生成器只消费 HD 位。";
}

function select(mesh) {
  for (const item of state.interactive) {
    item.material.emissive.setHex(item === mesh ? 0xffb44a : 0x3a2a0c);
  }
  state.selected = mesh;
  const box = $("inspect");
  box.hidden = !mesh;
  if (!mesh) return;
  $("inspect-id").textContent = mesh.userData.id;
  $("inspect-tag").textContent = mesh.userData.tag;
  const info = state.manifest.interactables.find((item) => item.id === mesh.userData.id);
  $("inspect-hint").textContent = info?.hint ?? "该交互物尚未绑定蓝图。";
}

const raycaster = new THREE.Raycaster();
const pointer = new THREE.Vector2();
let dragging = null;

renderer.domElement.addEventListener("pointerdown", (event) => {
  dragging = { x: event.clientX, y: event.clientY,
               yaw, pitch, moved: 0 };
  renderer.domElement.setPointerCapture(event.pointerId);
});
renderer.domElement.addEventListener("pointermove", (event) => {
  if (!dragging) return;
  const dx = event.clientX - dragging.x;
  const dy = event.clientY - dragging.y;
  dragging.moved += Math.abs(dx) + Math.abs(dy);
  yaw = dragging.yaw - dx * 0.005;
  pitch = Math.min(1.42, Math.max(-0.12, dragging.pitch + dy * 0.004));
  updateCamera();
});
renderer.domElement.addEventListener("pointerup", (event) => {
  if (dragging && dragging.moved < 5) {
    pointer.set((event.clientX / innerWidth) * 2 - 1,
                -(event.clientY / innerHeight) * 2 + 1);
    raycaster.setFromCamera(pointer, camera);
    const hits = raycaster.intersectObjects(state.interactive, true);
    select(hits.length ? hits[0].object : null);
  }
  dragging = null;
});
renderer.domElement.addEventListener("wheel", (event) => {
  event.preventDefault();
  distance = Math.min(90, Math.max(12, distance + event.deltaY * 0.02));
  updateCamera();
}, { passive: false });

addEventListener("resize", () => {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});

$("lod").addEventListener("change", (event) => {
  state.lod = event.target.value;
  select(null);
  rebuild();
});
$("variant").addEventListener("change", async (event) => {
  try {
    state.variantFile = event.target.value;
    const entry = state.variants.variants.find((item) => item.file === state.variantFile);
    state.shape = entry ? entry.shape : state.manifest.grid.shape;
    state.parts = await fetchBoard(BUILD + state.variantFile);
    select(null);
    rebuild();
  } catch (error) {
    fail(`切换几何变体失败：${error.message}`);
  }
});
$("inspect-close").addEventListener("click", () => select(null));

function animate() {
  requestAnimationFrame(animate);
  renderer.render(scene, camera);
}

// 无头验证钩子：playwright / CDP 的 evaluate 里调用
// __zengine.probe() 可在同一任务内 render + readPixels（前缓冲默认不保留，
// 跨任务 readPixels 只会读到被清空的黑帧），并回传场景统计。
window.__zengine = {
  scene, camera, renderer, boardGroup,
  probe() {
    renderer.render(scene, camera);
    const gl = renderer.getContext();
    const width = gl.drawingBufferWidth;
    const height = gl.drawingBufferHeight;
    const pixel = new Uint8Array(4);
    gl.readPixels(Math.floor(width / 2), Math.floor(height / 2), 1, 1,
      gl.RGBA, gl.UNSIGNED_BYTE, pixel);
    let meshes = 0;
    let visible = 0;
    boardGroup.traverse((object) => {
      if (!object.isMesh) return;
      meshes += 1;
      if (object.visible) visible += 1;
    });
    return {
      pixel: [...pixel],
      canvas: [width, height],
      boardMeshes: meshes,
      boardVisible: visible,
      boardChildren: boardGroup.children.length,
      camera: camera.position.toArray().map((v) => +v.toFixed(1)),
      drawCalls: renderer.info.render.calls,
      triangles: renderer.info.render.triangles,
    };
  },
};

async function main() {
  if (location.protocol === "file:") {
    fail("请通过专用静态服务打开（fetch 在 file:// 下会被拦截；"
      + "python -m http.server 会把 .mjs 返回成 text/plain）：\n\n"
      + "python integration/chessboard/tools/serve.py\n\n"
      + "然后访问 http://127.0.0.1:8080/web/");
    return;
  }
  try {
    const [spec, manifest, variants, stylePack, visualPack] = await Promise.all([
      fetchJSON("../board-spec.json"),
      fetchJSON(BUILD + "board.manifest.json"),
      fetchJSON(BUILD + "variants.json"),
      fetchJSON("../style-pack.json"),
      fetchJSON("../visual-pack.json").catch(() => null),
    ]);
    state.spec = spec;
    state.manifest = manifest;
    state.variants = variants;
    state.stylePack = stylePack;
    state.visualPack = visualPack;
    state.parts = await fetchBoard(BUILD + "board.zbrd");
    state.shape = manifest.grid.shape;

    if (visualPack) {
      // 与 Unity Volume 的 TonemapMode / ExposureValue 保持同向（web 侧只是近似）。
      renderer.toneMapping = THREE.ACESFilmicToneMapping;
      renderer.toneMappingExposure = 1 + visualPack.post_processing.exposure_value;
    }
    // 风格包与 AI 视觉包由同一张概念图派生时常同名，去重避免 "X · A · A"。
    // stat 里仍会单独标注 AI 视觉层，信息不丢。
    const visualName = visualPack && visualPack.name !== stylePack.name
      ? ` · ${visualPack.name}` : "";
    $("skin-name").textContent = `${spec.name} · ${stylePack.name}${visualName}`;
    const variantSelect = $("variant");
    variantSelect.innerHTML = "";
    const option = document.createElement("option");
    option.value = "board.zbrd";
    option.textContent = `默认（${manifest.grid.shape}）`;
    variantSelect.appendChild(option);
    for (const item of variants.variants) {
      const entry = document.createElement("option");
      entry.value = item.file;
      entry.textContent = item.shape;
      variantSelect.appendChild(entry);
    }

    addLights();
    rebuild();
    await loadExternalMeshes();
    rebuild();
    animate();
    $("loading").classList.add("done");
  } catch (error) {
    fail(`加载棋盘失败：${error.message}\n\n`
      + "先在仓库根执行：python integration/chessboard/zboard.py --variants");
  }
}

main();
