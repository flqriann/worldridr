// Procedural generation of the built-in sounds (WAV) and textures (PNG),
// so the studio ships a usable library without third-party media.

import { bytesToBase64 } from '../core/encoding';

const RATE = 22050;

function wav(samples: Float32Array): string {
  const n = samples.length;
  const buf = new ArrayBuffer(44 + n * 2);
  const dv = new DataView(buf);
  const str = (o: number, s: string) => { for (let i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); dv.setUint32(4, 36 + n * 2, true); str(8, 'WAVE'); str(12, 'fmt ');
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
  dv.setUint32(24, RATE, true); dv.setUint32(28, RATE * 2, true); dv.setUint16(32, 2, true); dv.setUint16(34, 16, true);
  str(36, 'data'); dv.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) dv.setInt16(44 + i * 2, Math.max(-1, Math.min(1, samples[i])) * 32767, true);
  return bytesToBase64(new Uint8Array(buf));
}

function render(seconds: number, fn: (t: number, i: number) => number): string {
  const n = Math.floor(seconds * RATE);
  const s = new Float32Array(n);
  for (let i = 0; i < n; i++) s[i] = fn(i / RATE, i);
  return wav(s);
}

let seed = 1;
const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647 * 2 - 1; };

export const SYNTH_SOUNDS: Record<string, () => string> = {
  Jump: () => render(0.3, (t) => Math.sin(2 * Math.PI * (300 + 900 * t) * t) * Math.exp(-t * 9) * 0.6),
  Coin: () => render(0.35, (t) => (Math.sign(Math.sin(2 * Math.PI * (t < 0.08 ? 988 : 1319) * t)) * 0.25) * Math.exp(-t * 8)),
  Hit: () => { seed = 7; return render(0.25, (t) => rnd() * Math.exp(-t * 22) * 0.7 + Math.sin(2 * Math.PI * 90 * t) * Math.exp(-t * 15) * 0.5); },
  Explosion: () => {
    seed = 3; let lp = 0;
    return render(1.6, (t) => { lp += (rnd() - lp) * 0.08; return lp * 3 * Math.exp(-t * 2.5); });
  },
  Click: () => render(0.06, (t) => Math.sin(2 * Math.PI * 1800 * t) * Math.exp(-t * 80) * 0.6),
  Wind: () => {
    seed = 11; let lp = 0;
    return render(4, (t) => { lp += (rnd() - lp) * 0.02; const env = Math.min(1, t * 2, (4 - t) * 2); return lp * 4 * (0.6 + 0.4 * Math.sin(t * 2.2)) * env; });
  },
  Melody: () => {
    const notes = [523.25, 659.25, 783.99, 659.25, 587.33, 698.46, 880, 698.46];
    return render(4, (t) => {
      const k = Math.floor(t / 0.5), lt = t - k * 0.5;
      const f = notes[k % notes.length];
      return (Math.sin(2 * Math.PI * f * t) * 0.5 + Math.sin(2 * Math.PI * f * 2 * t) * 0.15) * Math.exp(-lt * 5) * 0.5;
    });
  },
  Fire: () => {
    seed = 5; let lp = 0;
    return render(3, (t) => { lp += (rnd() - lp) * 0.3; const crackle = Math.random() < 0.002 ? 1 : 0; return lp * 0.25 + crackle * rnd() * 0.8; });
  },
};

// ---- Textures --------------------------------------------------------------

type Painter = (ctx: CanvasRenderingContext2D, s: number) => void;

export const SYNTH_TEXTURES: Record<string, Painter> = {
  'Studs Grid': (c, s) => {
    c.fillStyle = '#ffffff'; c.fillRect(0, 0, s, s);
    c.strokeStyle = 'rgba(0,0,0,0.16)'; c.lineWidth = s / 64;
    c.strokeRect(0, 0, s, s);
    c.strokeStyle = 'rgba(0,0,0,0.06)'; c.lineWidth = s / 128;
    c.beginPath(); c.moveTo(s / 2, 0); c.lineTo(s / 2, s); c.moveTo(0, s / 2); c.lineTo(s, s / 2); c.stroke();
  },
  Checker: (c, s) => { for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) { c.fillStyle = (x + y) % 2 ? '#e8e8e8' : '#9a9a9a'; c.fillRect(x * s / 8, y * s / 8, s / 8, s / 8); } },
  Bricks: (c, s) => {
    c.fillStyle = '#c9c2b8'; c.fillRect(0, 0, s, s);
    const h = s / 8, w = s / 4;
    for (let r = 0; r < 8; r++) for (let k = -1; k < 5; k++) {
      const x = k * w + (r % 2 ? w / 2 : 0);
      c.fillStyle = `hsl(${8 + (k * 7 + r * 13) % 10}, 45%, ${38 + ((k * 31 + r * 17) % 12)}%)`;
      c.fillRect(x + 2, r * h + 2, w - 4, h - 4);
    }
  },
  Planks: (c, s) => {
    for (let i = 0; i < 6; i++) {
      c.fillStyle = `hsl(30, 45%, ${40 + (i * 37) % 14}%)`; c.fillRect(0, i * s / 6, s, s / 6);
      c.strokeStyle = 'rgba(40,20,5,.6)'; c.lineWidth = 2; c.strokeRect(0, i * s / 6, s, s / 6);
      c.strokeStyle = 'rgba(60,30,10,.25)'; c.lineWidth = 1;
      for (let g = 0; g < 5; g++) { c.beginPath(); const y = i * s / 6 + 4 + g * 7; c.moveTo(0, y); c.bezierCurveTo(s / 3, y + 3, (2 * s) / 3, y - 3, s, y); c.stroke(); }
    }
  },
  Stripes: (c, s) => { for (let i = 0; i < 8; i++) { c.fillStyle = i % 2 ? '#f2c230' : '#222'; c.beginPath(); c.moveTo(i * s / 4 - s, 0); c.lineTo(i * s / 4 - s + s / 8, 0); c.lineTo(i * s / 4 + s / 8, s); c.lineTo(i * s / 4, s); c.fill(); } },
  Dots: (c, s) => { c.fillStyle = '#2a6fd6'; c.fillRect(0, 0, s, s); c.fillStyle = '#fff'; for (let y = 0; y < 6; y++) for (let x = 0; x < 6; x++) { c.beginPath(); c.arc((x + 0.5 + (y % 2) * 0.5) * s / 6, (y + 0.5) * s / 6, s / 30, 0, Math.PI * 2); c.fill(); } },
  Grid: (c, s) => { c.fillStyle = '#20242b'; c.fillRect(0, 0, s, s); c.strokeStyle = '#3fd0ff'; c.lineWidth = 2; for (let i = 0; i <= 8; i++) { c.beginPath(); c.moveTo(i * s / 8, 0); c.lineTo(i * s / 8, s); c.moveTo(0, i * s / 8); c.lineTo(s, i * s / 8); c.stroke(); } },
  Tiles: (c, s) => { c.fillStyle = '#ddd8ce'; c.fillRect(0, 0, s, s); for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) { c.fillStyle = `hsl(200, 20%, ${78 + ((x * 7 + y * 3) % 8)}%)`; c.fillRect(x * s / 4 + 3, y * s / 4 + 3, s / 4 - 6, s / 4 - 6); } },
  Smiley: (c, s) => { c.fillStyle = '#ffd43b'; c.fillRect(0, 0, s, s); c.fillStyle = '#222'; c.beginPath(); c.arc(s * 0.35, s * 0.38, s * 0.06, 0, 7); c.arc(s * 0.65, s * 0.38, s * 0.06, 0, 7); c.fill(); c.lineWidth = s * 0.04; c.beginPath(); c.arc(s / 2, s / 2, s * 0.25, 0.2 * Math.PI, 0.8 * Math.PI); c.stroke(); },
};

export function renderTexture(name: string, size = 256): string {
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext('2d')!;
  SYNTH_TEXTURES[name](ctx, size);
  return canvas.toDataURL('image/png').split(',')[1];
}
