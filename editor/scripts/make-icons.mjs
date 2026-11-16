// Renders the app icons (512×512 PNG) without external tools:
//   build/icons/studio.png – blue hexagon with an "F"
//   build/icons/player.png – violet hexagon with a play triangle
// electron-builder converts them to .ico (Windows) and uses them for Linux.
import { mkdirSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const out = join(dirname(fileURLToPath(import.meta.url)), '..', 'build', 'icons');
const S = 512, SS = 4; // size, supersampling per axis

const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = (buf) => { let c = 0xffffffff; for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(rgba) {
  const raw = Buffer.alloc((S * 4 + 1) * S);
  for (let y = 0; y < S; y++) { raw[y * (S * 4 + 1)] = 0; rgba.copy(raw, y * (S * 4 + 1) + 1, y * S * 4, (y + 1) * S * 4); }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(S, 0); ihdr.writeUInt32BE(S, 4); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}

// point-in-polygon (even-odd)
const inPoly = (x, y, pts) => { let c = false; for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) { const [xi, yi] = pts[i], [xj, yj] = pts[j]; if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) c = !c; } return c; };
const hex = (cx, cy, r) => Array.from({ length: 6 }, (_, i) => { const a = Math.PI / 6 + (i * Math.PI) / 3; return [cx + r * Math.cos(a - Math.PI / 2 + Math.PI / 6 * 0), cy + r * Math.sin(a - Math.PI / 2)]; });
const lerp = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);

function render(top, bottom, glyph) {
  const outer = hex(256, 256, 236);
  const buf = Buffer.alloc(S * S * 4);
  for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
    let r = 0, g = 0, b = 0, a = 0;
    for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) {
      const px = x + (sx + 0.5) / SS, py = y + (sy + 0.5) / SS;
      if (!inPoly(px, py, outer)) continue;
      const c = glyph(px, py) ? [255, 255, 255] : lerp(top, bottom, Math.min(1, Math.max(0, (px + py) / (2 * S))));
      r += c[0]; g += c[1]; b += c[2]; a += 1;
    }
    const n = SS * SS, o = (y * S + x) * 4;
    if (a) { buf[o] = r / a; buf[o + 1] = g / a; buf[o + 2] = b / a; buf[o + 3] = (255 * a) / n; }
  }
  return png(buf);
}

// "F" from rectangles
const rect = (x, y, x0, y0, x1, y1) => x >= x0 && x <= x1 && y >= y0 && y <= y1;
const letterF = (x, y) => rect(x, y, 186, 140, 236, 372) || rect(x, y, 186, 140, 336, 188) || rect(x, y, 186, 236, 316, 282);
const play = (x, y) => inPoly(x, y, [[204, 150], [204, 362], [370, 256]]);

mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'studio.png'), render([79, 149, 255], [47, 70, 200], letterF));
writeFileSync(join(out, 'player.png'), render([150, 110, 255], [70, 40, 170], play));
console.log('[icons] build/icons/studio.png, build/icons/player.png');
