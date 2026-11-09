// 场景还原输入配置页逻辑：纯 DOM + 原生 API，不依赖 three.js / 任何框架。
// 职责：把表单收集成 restore-profile.json，上传截图算 sha256，导出 / 导入 / 预览。

"use strict";

const $ = (id) => document.getElementById(id);

function parseFloatList(text) {
  const parts = String(text || "").split(",").map((s) => s.trim()).filter(Boolean);
  return parts.map((s) => Number(s));
}

async function sha256Hex(bytes) {
  const buf = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(buf)).map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function fileToSha(file) {
  const buf = await file.arrayBuffer();
  return sha256Hex(buf);
}

// ---- 动态列表 ----

function addAnchorRow() {
  const div = document.createElement("div");
  div.className = "anchor-item";
  div.innerHTML = `
    <input type="text" class="a-id" placeholder="id 如 board_edge" style="flex:1">
    <select class="a-kind">
      <option value="board_edge">棋盘边长</option>
      <option value="cell_pitch">格距</option>
      <option value="object_height">物体高度</option>
      <option value="custom">自定义</option>
    </select>
    <input type="number" class="a-value" placeholder="米" step="0.01" style="width:100px">
    <span class="rm">删</span>`;
  div.querySelector(".rm").onclick = () => div.remove();
  $("anchor-list").appendChild(div);
}

function addViewRow() {
  const div = document.createElement("div");
  div.className = "view-item";
  div.innerHTML = `
    <input type="text" class="v-id" placeholder="角度 id 如 front" style="width:140px">
    <select class="v-angle">
      <option value="front">正面</option><option value="back">背面</option>
      <option value="left">左</option><option value="right">右</option>
      <option value="top">俯视</option><option value="perspective">透视</option>
      <option value="custom">自定义</option>
    </select>
    <input type="file" class="v-file" accept="image/*">
    <span class="rm">删</span>`;
  div.querySelector(".rm").onclick = () => div.remove();
  $("view-list").appendChild(div);
}

function addMatRow() {
  const div = document.createElement("div");
  div.className = "mat-item";
  div.innerHTML = `
    <input type="text" class="m-id" placeholder="id 如 core_slab" style="width:140px">
    <input type="text" class="m-target" placeholder="目标 如 台面/包边" style="flex:1">
    <input type="text" class="m-color" placeholder="#RRGGBB" style="width:110px">
    <input type="file" class="m-ref" accept="image/*" title="材质参考图">
    <span class="rm">删</span>`;
  div.querySelector(".rm").onclick = () => div.remove();
  $("mat-list").appendChild(div);
}

function addFxRow() {
  const div = document.createElement("div");
  div.className = "fx-item";
  div.innerHTML = `
    <input type="text" class="f-id" placeholder="id 如 lantern_glow" style="width:160px">
    <select class="f-kind">
      <option value="glow">光晕</option><option value="particle">粒子</option>
      <option value="flow">流光</option><option value="shimmer">闪烁</option>
      <option value="custom">自定义</option>
    </select>
    <label style="margin:0;display:inline-flex;gap:4px;align-items:center"><input type="checkbox" class="f-anim" checked> 动画</label>
    <span class="rm">删</span>`;
  div.querySelector(".rm").onclick = () => div.remove();
  $("fx-list").appendChild(div);
}

// ---- 收集 / 回填 ----

async function collect() {
  const anchors = Array.from($("anchor-list").querySelectorAll(".anchor-item"))
    .map((r) => ({
      id: r.querySelector(".a-id").value.trim(),
      kind: r.querySelector(".a-kind").value,
      value: Number(r.querySelector(".a-value").value),
      unit: "meter",
    }))
    .filter((a) => a.id && !Number.isNaN(a.value));

  const views = [];
  for (const r of $("view-list").querySelectorAll(".view-item")) {
    const file = r.querySelector(".v-file").files[0];
    const id = r.querySelector(".v-id").value.trim();
    if (!id || !file) continue;
    views.push({
      id,
      angle: r.querySelector(".v-angle").value,
      image_sha256: await fileToSha(file),
    });
  }

  const materials = [];
  for (const r of $("mat-list").querySelectorAll(".mat-item")) {
    const id = r.querySelector(".m-id").value.trim();
    if (!id) continue;
    const item = {
      id,
      target: r.querySelector(".m-target").value.trim(),
    };
    const color = r.querySelector(".m-color").value.trim();
    if (color) item.color_hex = color;
    const ref = r.querySelector(".m-ref").files[0];
    if (ref) item.reference_image_sha256 = await fileToSha(ref);
    materials.push(item);
  }

  const effects = Array.from($("fx-list").querySelectorAll(".fx-item"))
    .map((r) => ({
      id: r.querySelector(".f-id").value.trim(),
      kind: r.querySelector(".f-kind").value,
      animated: r.querySelector(".f-anim").checked,
    }))
    .filter((f) => f.id);

  const camPos = parseFloatList($("cam-pos").value);
  const camTarget = parseFloatList($("cam-target").value);

  const profile = {
    schema: "zengine.restore-profile/v1",
    id: "restore-config",
    name: $("skin-name").value.trim() || undefined,
    skin: {
      name: $("skin-name").value.trim() || "",
      identity_status: $("skin-status").value,
      source_game: $("skin-game").value,
      known_alias: $("skin-alias").value.split(",").map((s) => s.trim()).filter(Boolean),
    },
    reference: {
      scene_template: $("ref-scene").value.trim() || undefined,
      units: "meter",
      size_anchors: anchors,
      logical_grid: {
        columns: Number($("grid-cols").value) || undefined,
        rows: Number($("grid-rows").value) || undefined,
      },
    },
    views,
    camera: {
      fov: $("cam-fov").value === "" ? null : Number($("cam-fov").value),
      pitch_deg: $("cam-pitch").value === "" ? null : Number($("cam-pitch").value),
      roll_deg: $("cam-roll").value === "" ? null : Number($("cam-roll").value),
      position: camPos.length === 3 ? camPos : null,
      target: camTarget.length === 3 ? camTarget : null,
      locked: $("cam-locked").value === "true",
      orthographic: $("cam-ortho").value === "true",
    },
    baseline: {
      image_sha256: "",
      tolerance: {
        iou: Number($("tol-iou").value),
        pixel_delta: Number($("tol-pixel").value),
      },
    },
    appearance: {
      materials,
      lighting: {
        time_of_day: $("light-tod").value,
        color_temp_k: $("light-k").value === "" ? null : Number($("light-k").value),
        emissive_sources: [],
      },
      effects,
    },
    output: {
      target_form: $("out-form").value,
      reuse_existing_assets: false,
      reuse_asset_ids: $("out-reuse").value.split(",").map((s) => s.trim()).filter(Boolean),
      delivered_lod: $("out-lod").value,
    },
  };

  const baseFile = $("baseline-file").files[0];
  if (baseFile) {
    profile.baseline.image_sha256 = await fileToSha(baseFile);
  }

  return profile;
}

function renderWarnings(profile) {
  const w = [];
  if (profile.skin.identity_status !== "known") {
    w.push("皮肤身份未确认为「已知」，还原将仅按截图语义层进行。");
  }
  if ((profile.views || []).length < 2) {
    w.push("多角度截图不足 2 张，被遮挡背面无法推断，建议补正面 / 侧面 / 背面 / 俯视。");
  }
  if (!(profile.reference.size_anchors || []).length) {
    w.push("未配置尺寸锚点，尺度无法锚定到米制（可复用 7×8 逻辑格作标尺）。");
  }
  if (!profile.baseline.image_sha256) {
    w.push("未配置验收基准帧，L4 对拍缺少 ground truth。");
  }
  const box = $("warnings");
  if (w.length) {
    box.hidden = false;
    box.innerHTML = "<b>待补输入（影响还原精度，非阻断）</b><ul>" + w.map((x) => "<li>" + x + "</li>").join("") + "</ul>";
  } else {
    box.hidden = true;
  }
}

function syncPreview(profile) {
  $("json-preview").value = JSON.stringify(profile, null, 2);
  renderWarnings(profile);
}

async function exportProfile() {
  const profile = await collect();
  syncPreview(profile);
  const blob = new Blob([JSON.stringify(profile, null, 2)], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = (profile.id || "restore-profile") + ".json";
  a.click();
  URL.revokeObjectURL(a.href);
  $("status").textContent = "已导出 " + a.download;
}

async function importProfile() {
  const file = $("import-file").files[0];
  if (!file) return;
  const profile = JSON.parse(await file.text());
  fill(profile);
  $("status").textContent = "已导入";
}

function fill(profile) {
  const s = profile.skin || {};
  $("skin-name").value = s.name || "";
  $("skin-status").value = s.identity_status || "unknown";
  $("skin-game").value = s.source_game || "jcc";
  $("skin-alias").value = (s.known_alias || []).join(", ");
  const ref = profile.reference || {};
  $("ref-scene").value = ref.scene_template || "";
  $("grid-cols").value = (ref.logical_grid && ref.logical_grid.columns) || 7;
  $("grid-rows").value = (ref.logical_grid && ref.logical_grid.rows) || 8;
  const cam = profile.camera || {};
  $("cam-fov").value = cam.fov ?? 31;
  $("cam-pitch").value = cam.pitch_deg ?? 50;
  $("cam-roll").value = cam.roll_deg ?? 0;
  $("cam-pos").value = (cam.position || []).join(", ");
  $("cam-target").value = (cam.target || []).join(", ");
  $("cam-locked").value = cam.locked ? "true" : "false";
  $("cam-ortho").value = cam.orthographic ? "true" : "false";
  const b = profile.baseline || {};
  $("tol-iou").value = (b.tolerance && b.tolerance.iou) ?? 0.9;
  $("tol-pixel").value = (b.tolerance && b.tolerance.pixel_delta) ?? 0.05;
  const out = profile.output || {};
  $("out-form").value = out.target_form || "look_alike";
  $("out-lod").value = out.delivered_lod || "HD";
  $("out-reuse").value = (out.reuse_asset_ids || []).join(", ");
  const ap = profile.appearance || {};
  $("light-tod").value = (ap.lighting && ap.lighting.time_of_day) || "day";
  $("light-k").value = (ap.lighting && ap.lighting.color_temp_k) ?? "";
  syncPreview(profile);
}

async function copyJson() {
  await exportPreviewOnly();
  const text = $("json-preview").value;
  await navigator.clipboard.writeText(text);
  $("status").textContent = "已复制 JSON";
}

async function exportPreviewOnly() {
  $("json-preview").value = JSON.stringify(await collect(), null, 2);
}

// ---- 初始化 ----

function bind() {
  $("add-anchor").onclick = addAnchorRow;
  $("add-view").onclick = addViewRow;
  $("add-mat").onclick = addMatRow;
  $("add-fx").onclick = addFxRow;
  $("btn-export").onclick = exportProfile;
  $("btn-import").onclick = () => $("import-file").click();
  $("btn-copy").onclick = copyJson;
  $("import-file").onchange = importProfile;
  // 任何输入即时刷新预览（截图上传走异步，用 change 触发）
  document.querySelectorAll("input, select").forEach((el) => {
    el.addEventListener("input", () => { exportPreviewOnly().then(() => renderWarningsFromPreview()); });
    el.addEventListener("change", () => { exportPreviewOnly().then(() => renderWarningsFromPreview()); });
  });
  // 初始：用默认值渲染一次预览
  exportPreviewOnly().then(() => renderWarningsFromPreview());
}

async function renderWarningsFromPreview() {
  try {
    const p = JSON.parse($("json-preview").value);
    renderWarnings(p);
  } catch (e) { /* 尚未形成合法 JSON */ }
}

// 预热：默认给一个示例视图占位不需要；直接空列表
bind();
