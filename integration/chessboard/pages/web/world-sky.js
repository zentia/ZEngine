// 12 套视觉包共用的程序化天空。只有 starry-night 走 visual-pack.json 里 sha
// 锁定的全景图；其余 11 个世界用各自的 sky 调色板在 canvas 里画一张 2:1
// 全景，避免为了「定调」再往仓库塞 11 张外部贴图，也保证离线可复现。
//
// 画布是等距柱状风格图（不是严格相机投影），与 buildSky() 把 v 压到天空带
// 的做法一致；左右缝隙用「同一笔在 x-w / x / x+w 各画一次」抹平。

function parseHex(hex) {
  const value = parseInt(hex.slice(1), 16);
  return [(value >> 16) & 255, (value >> 8) & 255, value & 255];
}

function rgba(hex, alpha) {
  const [r, g, b] = parseHex(hex);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

function mix(a, b, t) {
  const x = parseHex(a);
  const y = parseHex(b);
  const channel = (i) => Math.round(x[i] + (y[i] - x[i]) * t)
    .toString(16).padStart(2, "0");
  return `#${channel(0)}${channel(1)}${channel(2)}`;
}

function lcg(seed) {
  let a = seed >>> 0;
  return () => {
    a = (Math.imul(a, 1664525) + 1013904223) >>> 0;
    return a / 4294967296;
  };
}

// 与 pack.index 绑定的确定性随机：同一个世界每次刷新得到完全一样的天空。
export function paintSky(canvas, pack) {
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  const sky = pack.sky;
  const random = lcg(20261106 + (pack.index ?? 0) * 7919);

  // buildSky() 把球面 v 压成 3.9 倍，低仰角相机只看得到画布 y≈0.3..1.0。
  // 所以主色带要落在这一区间，且不能让 horizon / bottom 之间出现硬断层。
  const gradient = ctx.createLinearGradient(0, 0, 0, h);
  gradient.addColorStop(0, sky.top);
  gradient.addColorStop(0.42, mix(sky.top, sky.horizon, 0.65));
  gradient.addColorStop(0.72, sky.horizon);
  gradient.addColorStop(1, mix(sky.horizon, sky.bottom, 0.6));
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, w, h);

  // 地平线附近的一层湿雾，把三段渐变接起来。
  const haze = ctx.createLinearGradient(0, h * 0.58, 0, h * 0.98);
  haze.addColorStop(0, rgba(sky.horizon, 0));
  haze.addColorStop(0.6, rgba(sky.horizon, 0.16));
  haze.addColorStop(1, rgba(sky.bottom, 0));
  ctx.fillStyle = haze;
  ctx.fillRect(0, h * 0.58, w, h * 0.40);

  if (sky.stars > 0) {
    const count = Math.round(320 * sky.stars);
    for (let i = 0; i < count; i += 1) {
      const x = random() * w;
      const y = random() * h * 0.46;
      const r = 0.5 + random() * 1.4;
      ctx.fillStyle = rgba("#ffffff", 0.25 + random() * 0.6 * sky.stars);
      ctx.beginPath();
      ctx.arc(x, y, r * (w / 2048), 0, Math.PI * 2);
      ctx.fill();
    }
  }

  if (sky.clouds > 0) {
    const cloudColor = mix(sky.horizon, "#ffffff", 0.55);
    const count = Math.round(58 * sky.clouds);
    for (let i = 0; i < count; i += 1) {
      const x = random() * w;
      const y = h * (0.18 + random() * 0.52);
      // 球面把水平方向拉宽约 4 倍：画布里画成竖向椭圆，屏幕上才是横向云团。
      // 必须是径向渐变的实心柔斑；stroke() 的椭圆贴到球面上会变成一圈圈
      // 空心线圈，看起来像泡泡而不是云（截图实测）。
      const rx = (26 + random() * 62) * (w / 2048);
      const ry = (80 + random() * 190) * (h / 1024);
      const tilt = (random() - 0.5) * 0.42;
      const alpha = (0.10 + random() * 0.22) * sky.clouds;
      for (const offset of [-w, 0, w]) {
        ctx.save();
        ctx.translate(x + offset, y);
        ctx.rotate(tilt);
        ctx.scale(rx / ry, 1);
        const cloud = ctx.createRadialGradient(0, 0, 0, 0, 0, ry);
        cloud.addColorStop(0, rgba(cloudColor, alpha));
        cloud.addColorStop(0.55, rgba(cloudColor, alpha * 0.45));
        cloud.addColorStop(1, rgba(cloudColor, 0));
        ctx.fillStyle = cloud;
        ctx.beginPath();
        ctx.arc(0, 0, ry, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();
      }
    }
  }

  if (sky.sun) {
    const { x, y, radius, color } = sky.sun;
    const sx = x * w;
    const sy = y * h;
    const r = radius * w;
    // 同样补偿球面 UV 的水平拉伸：日轮在画布里是竖椭圆，屏幕上才是圆的。
    const SUN_X_SQUEEZE = 0.28;
    for (const offset of [-w, 0, w]) {
      ctx.save();
      ctx.translate(sx + offset, sy);
      ctx.scale(SUN_X_SQUEEZE, 1);
      const glow = ctx.createRadialGradient(0, 0, 0, 0, 0, r * 6);
      glow.addColorStop(0, rgba(color, 0.92));
      glow.addColorStop(0.20, rgba(color, 0.38));
      glow.addColorStop(1, rgba(color, 0));
      ctx.fillStyle = glow;
      ctx.fillRect(-r * 6, -r * 6, r * 12, r * 12);
      ctx.fillStyle = rgba(color, 0.95);
      ctx.beginPath();
      ctx.arc(0, 0, r, 0, Math.PI * 2);
      ctx.fill();
      ctx.restore();
    }
    // 低太阳在雾里拉出的碎光带（日出/暮色世界）。
    if (y > 0.44) {
      for (let i = 0; i < 160; i += 1) {
        const yy = h * (0.57 + random() * 0.26);
        const xx = sx + (random() - 0.5) * r * 9;
        const len = (8 + random() * 62) * (w / 2048);
        ctx.strokeStyle = rgba(color, 0.04 + random() * 0.18);
        ctx.lineWidth = (1 + random() * 3) * (w / 2048);
        ctx.beginPath();
        ctx.moveTo(xx - len / 2, yy);
        ctx.lineTo(xx + len / 2, yy);
        ctx.stroke();
      }
    }
  }
  return canvas;
}

export function makeSkyPanorama(THREE, pack) {
  const canvas = document.createElement("canvas");
  canvas.width = 2048;
  canvas.height = 1024;
  paintSky(canvas, pack);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  return texture;
}

// 概念图还没生成的世界，用同一张程序化天空的小图当缩略图（不依赖外部文件）。
export function skyPreviewURL(pack) {
  const canvas = document.createElement("canvas");
  canvas.width = 256;
  canvas.height = 128;
  return paintSky(canvas, pack).toDataURL("image/jpeg", 0.86);
}
