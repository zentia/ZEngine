// 棋盘演出：宿主派发事件，棋盘只负责表现。
//
// 本模块刻意不依赖 three.js / DOM 之外的任何东西：横幅、文案、时长、事件历史
// 都是纯 DOM；3D 侧的镜头/粒子/灯光脉冲由 starry.js 通过 onEvent 回调实现，
// 入场镜头每绕到一个特色站点再通过 stage() 让横幅点名那处特色。
// 事件 id、payload 字段、时长、配色的唯一真源是根目录 ceremony.json。

export const CEREMONY_SCHEMA = "zengine.chessboard.ceremony/v1";

export const CEREMONY_EVENT_IDS = [
  "entrance", "round_win", "round_lose", "win_streak", "level_up", "easter_egg",
];

export function validateCeremonyDoc(doc) {
  if (doc?.schema !== CEREMONY_SCHEMA) {
    throw new Error(`ceremony.json schema 不匹配：${doc?.schema}`);
  }
  if (doc.frozen !== true) throw new Error("ceremony contract must be frozen");
  const ids = doc.events.map((event) => event.id);
  if (new Set(ids).size !== ids.length) throw new Error("duplicate ceremony event id");
  if (ids.length !== CEREMONY_EVENT_IDS.length
      || CEREMONY_EVENT_IDS.some((id) => !ids.includes(id))) {
    throw new Error("ceremony.json must define exactly the 6 frozen board events");
  }
  for (const event of doc.events) {
    if (!(event.duration_ms > 0)) throw new Error(`${event.id} needs duration_ms`);
    if (!Array.isArray(event.payload)) throw new Error(`${event.id} needs payload[]`);
    if (event.channels.some((c) => !doc.channels.includes(c))) {
      throw new Error(`${event.id} uses an unknown channel`);
    }
  }
  return doc;
}

function colorFor(id, palette) {
  return {
    entrance: palette.accent,
    round_win: palette.win,
    round_lose: palette.lose,
    win_streak: palette.streak,
    level_up: palette.level,
    easter_egg: palette.egg,
  }[id] ?? palette.accent;
}

function textFor(event, payload) {
  switch (event.id) {
    case "entrance":
      return { kicker: "BOARD ENTRANCE / 入场动画", title: "画境展开",
        detail: "环绕画境特色 · 方格亮起 · 两位棋手就位" };
    case "round_win":
      return { kicker: "ROUND WON / 回合胜利", title: "回合胜利",
        detail: `第 ${payload.round ?? "—"} 回合 · 星屑上扬` };
    case "round_lose":
      return { kicker: "ROUND LOST / 回合失败", title: "回合失败",
        detail: `第 ${payload.round ?? "—"} 回合 · 灯火渐隐` };
    case "win_streak":
      return { kicker: "WIN STREAK / 连续胜利", title: `${payload.streak ?? "—"} 连胜`,
        detail: "连胜数由对局传入 · 棋盘只负责庆贺" };
    case "level_up":
      return { kicker: "LEVEL UP / 升级播报", title: `LV.${payload.level ?? "—"}`,
        detail: "等级由对局传入 · 光圈沿方格扩散" };
    case "easter_egg":
      return { kicker: "EASTER EGG / 互动彩蛋", title: "发现彩蛋",
        detail: payload.hint ?? "点击棋手占位，或按 E 键" };
    default:
      return { kicker: event.label_en, title: event.label_zh, detail: "" };
  }
}

export function createCeremony({ doc, root, palette = {}, onEvent = () => {} }) {
  validateCeremonyDoc(doc);
  const byId = new Map(doc.events.map((event) => [event.id, event]));
  const colors = { ...doc.palette_defaults, ...palette };
  const kicker = root.querySelector(".ceremony-kicker");
  const title = root.querySelector(".ceremony-title");
  const detail = root.querySelector(".ceremony-detail");
  const meter = root.querySelector(".ceremony-meter i");
  let last = null;
  let active = null;
  let history = [];
  let timer = null;
  let seq = 0;

  function stop() {
    if (timer) clearTimeout(timer);
    timer = null;
    active = null;
    root.classList.remove("show");
    root.removeAttribute("data-event");
  }

  // 入场环绕按站点切换文案：只改 kicker / detail，不动标题、时长与事件历史，
  // 冻结契约里的 entrance 仍然是同一个事件。
  function stage(kickerText, detailText) {
    if (active !== "entrance") return false;
    kicker.textContent = kickerText;
    detail.textContent = detailText;
    // 每一站重新触发淡入，三段看点不会像硬切字幕。
    detail.classList.remove("stage-flash");
    void detail.offsetWidth;
    detail.classList.add("stage-flash");
    return true;
  }

  function dispatch(id, payload = {}) {
    const event = byId.get(id);
    if (!event) return false;
    stop();
    const text = textFor(event, payload);
    kicker.textContent = text.kicker;
    title.textContent = text.title;
    detail.textContent = text.detail;
    root.style.setProperty("--ceremony-accent", colorFor(id, colors));
    meter.style.setProperty("--ceremony-duration", `${event.duration_ms}ms`);
    root.dataset.event = id;
    // 重新触发 CSS 动画：同一事件连播时必须强制一次 reflow。
    void root.offsetWidth;
    root.classList.add("show");
    active = id;
    last = { id, payload: { ...payload }, seq: ++seq };
    history.push(last);
    if (history.length > 32) history.shift();
    onEvent(event, payload);
    timer = setTimeout(stop, event.duration_ms);
    return true;
  }

  return {
    dispatch, stop, stage,
    last: () => (last ? { ...last } : null),
    active: () => active,
    history: () => [...history],
    eventIds: () => [...byId.keys()],
    duration: (id) => byId.get(id)?.duration_ms ?? 0,
  };
}
