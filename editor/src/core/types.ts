// Value types shared by the scene model, serializer, UI and script bridge.
// All value objects are treated as immutable: changing a property always
// assigns a new object, which keeps undo/redo and change detection simple.

export interface Vec3 { readonly x: number; readonly y: number; readonly z: number }
/** Roblox-style UDim2: scale (0..1 of parent) + pixel offset per axis. */
export interface UDim2 { readonly xs: number; readonly xo: number; readonly ys: number; readonly yo: number }
/** Colors are stored as lowercase "#rrggbb" strings. */
export type Color = string;

export const v3 = (x = 0, y = 0, z = 0): Vec3 => ({ x, y, z });
export const udim2 = (xs = 0, xo = 0, ys = 0, yo = 0): UDim2 => ({ xs, xo, ys, yo });

export function v3Equal(a: Vec3, b: Vec3, eps = 1e-9) {
  return Math.abs(a.x - b.x) < eps && Math.abs(a.y - b.y) < eps && Math.abs(a.z - b.z) < eps;
}

export function isVec3(v: unknown): v is Vec3 {
  return !!v && typeof v === 'object' && 'x' in v && 'y' in v && 'z' in v;
}
export function isUDim2(v: unknown): v is UDim2 {
  return !!v && typeof v === 'object' && 'xs' in v && 'yo' in v;
}

/** Structural equality for property values (numbers, strings, Vec3, UDim2, refs). */
export function valueEqual(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (isVec3(a) && isVec3(b)) return v3Equal(a, b);
  if (isUDim2(a) && isUDim2(b)) return a.xs === b.xs && a.xo === b.xo && a.ys === b.ys && a.yo === b.yo;
  return false;
}

export function cloneValue<T>(v: T): T {
  if (v && typeof v === 'object') return { ...(v as object) } as T;
  return v;
}

export function clamp(v: number, lo: number, hi: number) { return Math.min(hi, Math.max(lo, v)); }

export function hexToRgb(hex: Color): [number, number, number] {
  const h = hex.replace('#', '');
  const n = parseInt(h.length === 3 ? h.split('').map((c) => c + c).join('') : h, 16);
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}
export function rgbToHex(r: number, g: number, b: number): Color {
  const c = (x: number) => Math.round(clamp(x, 0, 1) * 255).toString(16).padStart(2, '0');
  return `#${c(r)}${c(g)}${c(b)}`;
}
export function normalizeColor(input: string): Color | null {
  const s = input.trim();
  if (/^#?[0-9a-f]{6}$/i.test(s)) return ('#' + s.replace('#', '')).toLowerCase();
  if (/^#?[0-9a-f]{3}$/i.test(s)) { const [r, g, b] = hexToRgb(s); return rgbToHex(r, g, b); }
  const m = s.match(/^(\d+)\s*,\s*(\d+)\s*,\s*(\d+)$/);
  if (m) return rgbToHex(+m[1] / 255, +m[2] / 255, +m[3] / 255);
  return null;
}

let idCounter = 0;
export function newId(prefix = ''): string {
  idCounter = (idCounter + 1) % 0xffffff;
  return prefix + Date.now().toString(36) + Math.random().toString(36).slice(2, 8) + idCounter.toString(36);
}

export function formatNum(n: number, digits = 3): string {
  if (!Number.isFinite(n)) return String(n);
  const r = Number(n.toFixed(digits));
  return Object.is(r, -0) ? '0' : String(r);
}
