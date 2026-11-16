// Pure geometry for worldridr world objects: building / road mesh generation
// (typed arrays, no scene objects) and polygon helpers.

import * as THREE from 'three';

export interface MeshArrays { pos: Float32Array; nrm: Float32Array; col: Float32Array }

export const ROAD_TYPES = [
  { name: 'Stadtstraße', hw: 3.5, lanes: 2, sidewalk: true, speed: 50 },
  { name: 'Landstraße', hw: 3.7, lanes: 2, sidewalk: false, speed: 100 },
  { name: '4 Spuren', hw: 7.2, lanes: 4, sidewalk: true, speed: 70 },
  { name: 'Einbahn', hw: 2.4, lanes: 1, sidewalk: true, speed: 30 },
  { name: 'Autobahn', hw: 5.6, lanes: 2, sidewalk: false, speed: 130 },
  { name: 'Feldweg', hw: 1.6, lanes: 1, sidewalk: false, speed: 30 },
] as const;

export function roadTypeIndex(name: string): number {
  const i = ROAD_TYPES.findIndex((r) => r.name === name);
  return i < 0 ? 0 : i;
}

const tmpCol = new THREE.Color();
export function hexRgb(hex: string): [number, number, number] {
  tmpCol.set(hex || '#cccccc');
  return [tmpCol.r, tmpCol.g, tmpCol.b];
}

/** Growing buffers of flat-shaded triangles. */
export class Builder {
  pos: number[] = [];
  nrm: number[] = [];
  col: number[] = [];
  tri(a: number[], b: number[], c: number[], rgb: number[], shade = 1) {
    const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const l = Math.hypot(nx, ny, nz) || 1;
    nx /= l; ny /= l; nz /= l;
    for (const p of [a, b, c]) {
      this.pos.push(p[0], p[1], p[2]);
      this.nrm.push(nx, ny, nz);
      this.col.push(rgb[0] * shade, rgb[1] * shade, rgb[2] * shade);
    }
  }
  quad(a: number[], b: number[], c: number[], d: number[], rgb: number[], shade = 1) {
    this.tri(a, b, c, rgb, shade);
    this.tri(a, c, d, rgb, shade);
  }
  done(): MeshArrays { return { pos: new Float32Array(this.pos), nrm: new Float32Array(this.nrm), col: new Float32Array(this.col) }; }
}

/** Signed area of a flat x,z polygon. */
export function signedArea(poly: number[]): number {
  let a = 0;
  const n = poly.length / 2;
  for (let i = 0; i < n; i++) {
    const j = (i + 1) % n;
    a += poly[i * 2] * poly[j * 2 + 1] - poly[j * 2] * poly[i * 2 + 1];
  }
  return a / 2;
}

export function parseNumberList(text: string): number[] {
  try {
    const a = JSON.parse(text);
    return Array.isArray(a) ? a.filter((x) => typeof x === 'number' && Number.isFinite(x)) : [];
  } catch { return []; }
}

export interface BuildingSpec {
  footprint: number[];       // normalized x,z pairs (-0.5..0.5)
  size: [number, number, number];
  kind: string;
  roofShape: string;
  roofHeight: number;
  wall: string;
  roof: string;
}

/** Ear-clipping triangulation. Returns index triples whose normal faces +y (up). */
export function triangulate(poly: number[]): number[][] {
  const pts: THREE.Vector2[] = [];
  for (let i = 0; i < poly.length; i += 2) pts.push(new THREE.Vector2(poly[i], poly[i + 1]));
  const tris = THREE.ShapeUtils.triangulateShape(pts, []);
  return tris.map((t) => {
    const a = pts[t[0]], b = pts[t[1]], c = pts[t[2]];
    // in (x,z): positive cross means the normal points to -y, so flip
    const cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    return cross > 0 ? [t[0], t[2], t[1]] : t;
  });
}

/**
 * Local-space geometry (origin = part center, y from -h/2 to +h/2) of a
 * building / water / area: walls plus a flat or pitched roof. Triangles face outward.
 */
export function buildBuilding(s: BuildingSpec): MeshArrays {
  const b = new Builder();
  let fp = s.footprint.length >= 6 ? s.footprint.slice() : [-0.5, -0.5, 0.5, -0.5, 0.5, 0.5, -0.5, 0.5];
  const [sx, sh, sz] = s.size;
  // wall winding: with +x east / +z south, a positive signed area is clockwise seen from above
  if (signedArea(fp) < 0) {
    const r: number[] = [];
    for (let i = fp.length / 2 - 1; i >= 0; i--) r.push(fp[i * 2], fp[i * 2 + 1]);
    fp = r;
  }
  const n = fp.length / 2;
  const P = (i: number) => [fp[i * 2] * sx, fp[i * 2 + 1] * sz];
  const wall = hexRgb(s.wall), roof = hexRgb(s.roof);
  const yb = -sh / 2;
  const flat = s.kind !== 'Building';
  const rh = flat ? 0 : Math.min(Math.max(0, s.roofHeight), sh * 0.9);
  const yw = yb + sh - rh;
  const yt = yb + sh;
  const topCol = flat ? wall : roof;

  for (let i = 0; i < n; i++) {
    const [ax, az] = P(i), [bx, bz] = P((i + 1) % n);
    b.quad([ax, yb, az], [bx, yb, bz], [bx, yw, bz], [ax, yw, az], wall, 0.94);
  }

  const poly = fp.map((v, i) => (i % 2 ? v * sz : v * sx));
  const tri = triangulate(poly);
  const shape = flat ? 'Flat' : s.roofShape;
  if (shape === 'Flat' || rh < 0.05 || n > 8) {
    for (const t of tri) b.tri([...P(t[0]).slice(0, 1), yt, P(t[0])[1]], [P(t[1])[0], yt, P(t[1])[1]], [P(t[2])[0], yt, P(t[2])[1]], topCol);
  } else if (shape === 'Pyramid') {
    let cx = 0, cz = 0;
    for (let i = 0; i < n; i++) { cx += P(i)[0]; cz += P(i)[1]; }
    cx /= n; cz /= n;
    for (let i = 0; i < n; i++) {
      const [ax, az] = P(i), [bx, bz] = P((i + 1) % n);
      b.tri([ax, yw, az], [bx, yw, bz], [cx, yt, cz], roof, i % 2 ? 0.85 : 1);
    }
  } else {
    let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
    for (let i = 0; i < n; i++) { const [x, z] = P(i); x0 = Math.min(x0, x); x1 = Math.max(x1, x); z0 = Math.min(z0, z); z1 = Math.max(z1, z); }
    const alongX = x1 - x0 >= z1 - z0;
    const mid = alongX ? (z0 + z1) / 2 : (x0 + x1) / 2;
    const height = (x: number, z: number) => {
      if (shape === 'Skillion') {
        const t = alongX ? (z - z0) / (z1 - z0 || 1) : (x - x0) / (x1 - x0 || 1);
        return yw + rh * t;
      }
      const half = alongX ? (z1 - z0) / 2 : (x1 - x0) / 2;
      const d = Math.abs((alongX ? z : x) - mid);
      return yt - rh * Math.min(1, d / (half || 1));
    };
    for (const t of splitAtRidge(poly, tri, alongX, mid, shape === 'Gabled')) {
      b.tri([t[0], height(t[0], t[1]), t[1]], [t[2], height(t[2], t[3]), t[3]], [t[4], height(t[4], t[5]), t[5]], roof, 1);
    }
    // wall infill up to the sloped roof line (gable ends)
    for (let i = 0; i < n; i++) {
      const [ax, az] = P(i), [bx, bz] = P((i + 1) % n);
      const ha = height(ax, az), hb = height(bx, bz);
      if (ha > yw + 0.01 || hb > yw + 0.01) b.quad([ax, yw, az], [bx, yw, bz], [bx, hb, bz], [ax, ha, az], wall, 0.9);
    }
  }
  return b.done();
}

/** Splits triangles that straddle the ridge line so the roof pitch is exact. */
function splitAtRidge(poly: number[], tris: number[][], alongX: boolean, mid: number, split: boolean): number[][] {
  const out: number[][] = [];
  const at = (i: number) => [poly[i * 2], poly[i * 2 + 1]];
  const side = (p: number[]) => (alongX ? p[1] : p[0]) - mid;
  const cut = (p: number[], q: number[]) => {
    const sp = side(p), sq = side(q);
    const t = sp / (sp - sq);
    return [p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t];
  };
  for (const t of tris) {
    const v = [at(t[0]), at(t[1]), at(t[2])];
    const s = v.map(side);
    const straddle = s.some((x) => x > 1e-6) && s.some((x) => x < -1e-6);
    if (!split || !straddle) { out.push([...v[0], ...v[1], ...v[2]]); continue; }
    let lone = 0;
    for (let k = 0; k < 3; k++) if (Math.sign(s[k]) !== Math.sign(s[(k + 1) % 3]) && Math.sign(s[k]) !== Math.sign(s[(k + 2) % 3])) lone = k;
    const a = v[lone], b = v[(lone + 1) % 3], c = v[(lone + 2) % 3];
    const pb = cut(a, b), pc = cut(a, c);
    out.push([...a, ...pb, ...pc], [...pb, ...b, ...c], [...pb, ...c, ...pc]);
  }
  return out;
}

export function bounds2(poly: number[]) {
  let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
  for (let i = 0; i < poly.length; i += 2) { x0 = Math.min(x0, poly[i]); x1 = Math.max(x1, poly[i]); z0 = Math.min(z0, poly[i + 1]); z1 = Math.max(z1, poly[i + 1]); }
  return { x0, x1, z0, z1 };
}

const round = (v: number, d: number) => { const m = 10 ** d; return Math.round(v * m) / m; };

/**
 * Fits a footprint given in world meters (x,z): yaw that aligns the dominant
 * edge direction with the local axes, world center of the local bounding box,
 * local extents and the normalized footprint.
 */
export function fitFootprint(world: number[]): { cx: number; cz: number; yaw: number; sx: number; sz: number; norm: number[] } {
  const n = world.length / 2;
  let bx = 0, bz = 0;
  for (let i = 0; i < n; i++) {
    const j = (i + 1) % n;
    const dx = world[j * 2] - world[i * 2], dz = world[j * 2 + 1] - world[i * 2 + 1];
    const l = Math.hypot(dx, dz);
    const a = Math.atan2(dz, dx) * 4;
    bx += Math.cos(a) * l; bz += Math.sin(a) * l;
  }
  const theta = Math.atan2(bz, bx) / 4;
  const c = Math.cos(theta), s = Math.sin(theta);
  const loc: number[] = [];
  for (let i = 0; i < n; i++) {
    const x = world[i * 2], z = world[i * 2 + 1];
    loc.push(x * c + z * s, -x * s + z * c);
  }
  const bb = bounds2(loc);
  const sx = Math.max(bb.x1 - bb.x0, 0.5), sz = Math.max(bb.z1 - bb.z0, 0.5);
  const mx = (bb.x0 + bb.x1) / 2, mz = (bb.z0 + bb.z1) / 2;
  const norm: number[] = [];
  for (let i = 0; i < n; i++) norm.push(round((loc[i * 2] - mx) / sx, 5), round((loc[i * 2 + 1] - mz) / sz, 5));
  const cx = mx * c - mz * s, cz = mx * s + mz * c;
  // Three rotation about +y by a maps local x to (cos a, 0, -sin a), so yaw = -theta
  return { cx, cz, yaw: (-theta * 180) / Math.PI, sx, sz, norm };
}

/** Road ribbon (world space) along an already-draped polyline [x,y,z,...]. */
export function buildRoadRibbon(line: number[], typeIdx: number): MeshArrays {
  const T = ROAD_TYPES[typeIdx] ?? ROAD_TYPES[0];
  const b = new Builder();
  const n = line.length / 3;
  if (n < 2) return b.done();
  const asphalt = hexRgb(typeIdx === 5 ? '#8a7656' : '#3a3c41');
  const walk = hexRgb('#9d9d98'), mark = hexRgb('#d9d6c8');
  const nx: number[] = [], nz: number[] = [];
  for (let i = 0; i < n; i++) {
    const a = Math.max(0, i - 1), c = Math.min(n - 1, i + 1);
    let dx = line[c * 3] - line[a * 3], dz = line[c * 3 + 2] - line[a * 3 + 2];
    const l = Math.hypot(dx, dz) || 1;
    dx /= l; dz /= l;
    nx.push(-dz); nz.push(dx);
  }
  const strip = (o0: number, o1: number, dy: number, rgb: number[]) => {
    for (let i = 0; i < n - 1; i++) {
      const p = (k: number, o: number) => [line[k * 3] + nx[k] * o, line[k * 3 + 1] + dy, line[k * 3 + 2] + nz[k] * o];
      // forward = (dx,dz), left offset n = (-dz,dx); this order yields an upward normal
      b.quad(p(i, o0), p(i, o1), p(i + 1, o1), p(i + 1, o0), rgb);
    }
  };
  strip(-T.hw, T.hw, 0.06, asphalt);
  if (T.sidewalk) {
    strip(-T.hw - 2, -T.hw, 0.16, walk);
    strip(T.hw, T.hw + 2, 0.16, walk);
  }
  if (T.lanes >= 2) strip(-0.07, 0.07, 0.075, mark);
  return b.done();
}
