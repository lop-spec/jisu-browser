// Verify the generated multi-resolution ICO without third-party packages.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
const icoUrl = new URL('./app.ico', import.meta.url);
const before = fs.readFileSync(icoUrl);
execFileSync(process.execPath, [fileURLToPath(new URL('./make-icon.mjs', import.meta.url))], { windowsHide: true });
const ico = fs.readFileSync(icoUrl);
assert.deepEqual(ico, before, 'checked-in ICO must reproduce exactly');
assert.equal(ico.readUInt16LE(0), 0);
assert.equal(ico.readUInt16LE(2), 1);
const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
assert.equal(ico.readUInt16LE(4), sizes.length);
let expectedOffset = 6 + 16 * sizes.length;
for (const [i, size] of sizes.entries()) {
  const e = 6 + 16 * i;
  assert.equal(ico[e] || 256, size);
  assert.equal(ico[e + 1] || 256, size);
  assert.equal(ico.readUInt16LE(e + 4), 1);
  assert.equal(ico.readUInt16LE(e + 6), 32);
  const length = ico.readUInt32LE(e + 8);
  const offset = ico.readUInt32LE(e + 12);
  assert.equal(offset, expectedOffset);
  expectedOffset += length;
  const png = ico.subarray(offset, offset + length);
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.equal(png.readUInt32BE(16), size);
  assert.equal(png.readUInt32BE(20), size);
  const data = [];
  for (let p = 8; p < png.length;) {
    const len = png.readUInt32BE(p);
    if (png.toString('ascii', p + 4, p + 8) === 'IDAT') data.push(png.subarray(p + 8, p + 8 + len));
    p += len + 12;
  }
  const raw = zlib.inflateSync(Buffer.concat(data));
  assert.equal(raw.length, (size * 4 + 1) * size);
  const pixel = (x,y) => raw.subarray(y * (size * 4 + 1) + 1 + x * 4, y * (size * 4 + 1) + 5 + x * 4);
  let orange = 0;
  for (let y = 0; y < size; y++) {
    assert.equal(raw[y * (size * 4 + 1)], 0);
    for (let x = 0; x < size; x++) {
      const p = pixel(x,y);
      if (p[0] === 255 && p[1] === 156 && p[2] === 85 && p[3] === 255) orange++;
      assert.equal(p[3], pixel(size - 1 - x,y)[3], 'circle alpha must be symmetric');
    }
  }
  assert.ok(orange > 0, `warm orange must remain visible at ${size}px`);
  for (const [x,y] of [[0,0],[0,size-1],[size-1,0],[size-1,size-1]]) assert.equal(pixel(x,y)[3], 0);
  assert.deepEqual([...pixel(Math.floor(size/4),Math.floor(size/2))], [27,27,27,255]);
}
assert.equal(expectedOffset, ico.length);
assert.match(fs.readFileSync(new URL('./app.svg', import.meta.url), 'utf8'), /<circle[^>]+fill="#1b1b1b"/);
console.log(`PASS: reproducible ICO; transparent circle + warm orange; ${sizes.join(', ')} px`);
