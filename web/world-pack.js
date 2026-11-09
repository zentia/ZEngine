// 12 套视觉包的读取 / 合并 / 选择与 HUD 应用。
//
// worlds.json 用 defaults + 每个世界的 overrides，避免把同一套字段抄 12 遍；
// 这里做深合并，解析出来的 pack 才是页面消费的完整视觉包。数组整体替换，
// 不逐项合并，否则 7 色调色板会被旧世界的值垫在后面。

import { skyPreviewURL } from "./world-sky.js";

export const WORLD_PACK_SCHEMA = "zengine.chessboard.world-pack/v1";

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

export function mergeWorld(defaults, overrides) {
  const result = { ...defaults };
  for (const [key, value] of Object.entries(overrides ?? {})) {
    result[key] = isObject(value) && isObject(defaults[key])
      ? mergeWorld(defaults[key], value)
      : value;
  }
  return result;
}

export function resolveWorldPack(doc, id) {
  if (doc.schema !== WORLD_PACK_SCHEMA) {
    throw new Error(`worlds.json schema 不匹配：${doc.schema}`);
  }
  const world = doc.worlds.find((entry) => entry.id === id)
    ?? doc.worlds.find((entry) => entry.id === doc.default);
  if (!world) throw new Error("worlds.json 里没有可用世界");
  const pack = { ...mergeWorld(doc.defaults, world.overrides), ...world };
  delete pack.overrides;
  return pack;
}

export function selectedWorldId(doc) {
  const requested = new URLSearchParams(location.search).get("world");
  return doc.worlds.some((entry) => entry.id === requested)
    ? requested : doc.default;
}

export function applyWorldHud(doc, pack, $) {
  document.title = `ZEngine · ${pack.title_zh} / 沉浸式预览`;
  $("hud-eyebrow").textContent = pack.hud.eyebrow;
  $("hud-title").textContent = pack.title_zh;
  $("hud-sub").textContent = pack.hud.sub;
  $("hud-from").textContent = pack.hud.from;
  $("credits-line").textContent = pack.hud.footer;
  $("loading-sub").textContent = `${pack.title_en} · 天空 / 地面 / 棋盘 / 灯光`;

  const select = $("world-select");
  select.replaceChildren(...doc.worlds.map((entry) => {
    const option = document.createElement("option");
    option.value = entry.id;
    const label = `${String(entry.index).padStart(2, "0")} ${entry.title_zh}`;
    option.textContent = entry.tier === "high" ? `${label} · 高完成度` : label;
    return option;
  }));
  select.value = pack.id;

  const thumb = $("thumb");
  const image = thumb.querySelector("img");
  const caption = thumb.querySelector("span");
  if (pack.concept.status === "asset_ready" && pack.concept.image) {
    thumb.href = `../${pack.concept.image}`;
    thumb.target = "_blank";
    thumb.onclick = null;
    image.src = `../${pack.concept.image}`;
    caption.textContent = `CONCEPT / ${String(pack.index).padStart(2, "0")}`;
    thumb.title = `${pack.title_zh} · AI 概念设定图`;
  } else {
    // prompt_only：不引用尚不存在的外部文件，用程序化天空小图占位。
    thumb.href = "#";
    thumb.removeAttribute("target");
    thumb.onclick = (event) => event.preventDefault();
    image.src = skyPreviewURL(pack);
    caption.textContent = `WORLD / ${String(pack.index).padStart(2, "0")}`;
    thumb.title = `${pack.title_zh} · 视觉包定调（概念图待生成）`;
  }
  thumb.hidden = false;
}
