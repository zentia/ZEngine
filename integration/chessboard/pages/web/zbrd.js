// ZBRD v1 解析器：浏览器与 Node 共用，不依赖 DOM / three.js。
// 格式定义见 ../zboard.py 文件头。几何公式只在 Python 侧实现一次，
// 这里只负责读字节，避免 three.js 与 Unity 各写一套几何生成逻辑而漂移。

export const MAGIC = 0x4452425a;
export const VERSION = 1;

export const KIND = Object.freeze({
  GRID: 0,
  FRAME: 1,
  SKY: 2,
  DECORATION: 3,
  INTERACTABLE: 4,
});

export const KIND_NAMES = Object.freeze({
  0: "grid", 1: "frame", 2: "sky", 3: "decoration", 4: "interactable",
});

export const LOD_BITS = Object.freeze({ HD: 1, High: 2, Mid: 4, Low: 8 });
export const ALL_LODS = 0b1111;

class Reader {
  constructor(buffer) {
    this.view = new DataView(buffer);
    this.offset = 0;
    this.size = buffer.byteLength;
    this.decoder = new TextDecoder("utf-8");
  }
  need(bytes, what) {
    if (this.offset + bytes > this.size) {
      throw new Error(`ZBRD truncated while reading ${what}`);
    }
  }
  u32(what = "u32") {
    this.need(4, what);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }
  u8() {
    this.need(1, "u8");
    const value = this.view.getUint8(this.offset);
    this.offset += 1;
    return value;
  }
  string(what = "string") {
    const length = this.u32(`${what} length`);
    if (length > 1 << 20) throw new Error("ZBRD oversized string");
    this.need(length, what);
    const bytes = new Uint8Array(this.view.buffer,
      this.view.byteOffset + this.offset, length);
    this.offset += length;
    return this.decoder.decode(bytes);
  }
  floats(count, what = "floats") {
    this.need(count * 4, what);
    if (count === 0) return new Float32Array(0);
    const out = new Float32Array(count);
    for (let i = 0; i < count; i += 1) {
      out[i] = this.view.getFloat32(this.offset, true);
      this.offset += 4;
    }
    return out;
  }
  uints(count, what = "indices") {
    this.need(count * 4, what);
    if (count === 0) return new Uint32Array(0);
    const out = new Uint32Array(count);
    for (let i = 0; i < count; i += 1) {
      out[i] = this.view.getUint32(this.offset, true);
      this.offset += 4;
    }
    return out;
  }
}

/** @param {ArrayBuffer} buffer @returns {Array<object>} */
export function parseZBRD(buffer) {
  const reader = new Reader(buffer);
  const magic = reader.u32("magic");
  const version = reader.u32("version");
  const partCount = reader.u32("part_count");
  const flags = reader.u32("flags");
  if (magic !== MAGIC) throw new Error("bad ZBRD magic");
  if (version !== VERSION) throw new Error(`unsupported ZBRD version ${version}`);
  if (flags !== 0) throw new Error("unsupported ZBRD flags");

  const parts = [];
  for (let i = 0; i < partCount; i += 1) {
    const name = reader.string("name");
    const kind = reader.u8();
    const lodMask = reader.u8();
    reader.u8();
    reader.u8();
    const material = reader.string("material");
    const tag = reader.string("tag");
    const vertexCount = reader.u32("vertex_count");
    const indexCount = reader.u32("index_count");
    const matrix = reader.floats(16, "matrix");
    const color = reader.floats(4, "color");
    const positions = reader.floats(vertexCount * 3, "positions");
    const normals = reader.floats(vertexCount * 3, "normals");
    const uvs = reader.floats(vertexCount * 2, "uvs");
    const indices = reader.uints(indexCount, "indices");
    if (indices.some((v) => v >= vertexCount)) {
      throw new Error(`part ${name} has out-of-range indices`);
    }
    parts.push({
      name, kind, lodMask, material, tag, matrix, color,
      positions, normals, uvs, indices, vertexCount, indexCount,
    });
  }
  if (reader.offset !== reader.size) {
    throw new Error("ZBRD has trailing bytes after last part");
  }
  return parts;
}

export function filterByLod(parts, lodName) {
  const bit = LOD_BITS[lodName];
  if (!bit) throw new Error(`unknown LOD ${lodName}`);
  return parts.filter((part) => (part.lodMask & bit) !== 0);
}

/** three.js 的 BufferGeometry 属性（法线/UV 已按 ZBRD 原样给出）。 */
export function toGeometry(part) {
  return { positions: part.positions, normals: part.normals,
           uvs: part.uvs, indices: part.indices };
}
