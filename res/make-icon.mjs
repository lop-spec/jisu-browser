// 生成 res/app.ico 与 app.svg：深色正圆 + 暖橙闪电，圆外透明。纯 Node，无依赖。
// node res/make-icon.mjs [预览 PNG 输出目录]
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const BACKGROUND = [0x1b, 0x1b, 0x1b];
const ORANGE = [0xff, 0x9c, 0x55];
const RADIUS = 0.46;
const BOLT = [[0.555, 0.19], [0.325, 0.54], [0.455, 0.54], [0.455, 0.81], [0.695, 0.45], [0.555, 0.45]];

function insideCircle(x, y) {
  return (x - 0.5) ** 2 + (y - 0.5) ** 2 <= RADIUS ** 2;
}

function insideBolt(x, y) {
  let inside = false;
  for (let i = 0, j = BOLT.length - 1; i < BOLT.length; j = i++) {
    const [xi, yi] = BOLT[i], [xj, yj] = BOLT[j];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

function render(size) {
  const ss = size <= 32 ? 8 : 4;  // 小尺寸多采样，边缘更顺
  const rgba = Buffer.alloc(size * size * 4);
  for (let py = 0; py < size; py++) {
    for (let px = 0; px < size; px++) {
      let bg = 0, bolt = 0;
      for (let sy = 0; sy < ss; sy++) {
        for (let sx = 0; sx < ss; sx++) {
          const x = (px + (sx + 0.5) / ss) / size;
          const y = (py + (sy + 0.5) / ss) / size;
          if (insideCircle(x, y)) {
            bg++;
            if (insideBolt(x, y)) bolt++;
          }
        }
      }
      const n = ss * ss;
      const a = bg / n;
      const w = bg ? bolt / bg : 0;
      const o = (py * size + px) * 4;
      for (let k = 0; k < 3; k++) rgba[o + k] = bg ? Math.round(BACKGROUND[k] * (1 - w) + ORANGE[k] * w) : 0;
      rgba[o + 3] = Math.round(a * 255);
    }
  }
  return rgba;
}

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const head = Buffer.alloc(8);
  head.writeUInt32BE(data.length, 0);
  head.write(type, 4, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
  return Buffer.concat([head, data, crc]);
}

function png(size) {
  const rgba = render(size);
  const raw = Buffer.alloc((size * 4 + 1) * size);
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

const images = SIZES.map(png);
const header = Buffer.alloc(6 + 16 * SIZES.length);
header.writeUInt16LE(0, 0);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(SIZES.length, 4);
let offset = header.length;
SIZES.forEach((size, i) => {
  const e = 6 + i * 16;
  header[e] = size >= 256 ? 0 : size;
  header[e + 1] = size >= 256 ? 0 : size;
  header[e + 2] = 0;
  header[e + 3] = 0;
  header.writeUInt16LE(1, e + 4);
  header.writeUInt16LE(32, e + 6);
  header.writeUInt32LE(images[i].length, e + 8);
  header.writeUInt32LE(offset, e + 12);
  offset += images[i].length;
});
const out = path.join(here, 'app.ico');
fs.writeFileSync(out, Buffer.concat([header, ...images]));
console.log('wrote', out, offset, 'bytes');

const hex = rgb => '#' + rgb.map(v => v.toString(16).padStart(2, '0')).join('');
const points = BOLT.map(([x, y]) => `${x * 256},${y * 256}`).join(' ');
fs.writeFileSync(path.join(here, 'app.svg'), `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">\n  <circle cx="128" cy="128" r="${RADIUS * 256}" fill="${hex(BACKGROUND)}"/>\n  <polygon points="${points}" fill="${hex(ORANGE)}"/>\n</svg>\n`);
if (process.argv[2]) {
  const preview = path.resolve(process.argv[2]);
  fs.mkdirSync(preview, { recursive: true });
  SIZES.forEach((size, i) => fs.writeFileSync(path.join(preview, `icon-${size}.png`), images[i]));
}
