// Real elevation data: AWS Terrain Tiles (Terrarium encoding, the same source the
// game uses) sampled into a Forge heightmap around a place.

import { TerrainData } from '../terrain/TerrainData';
import { httpBinary } from './net';
import { latToMy, lonToMx, toLatLon, type GeoRef } from './geo';

const TILE = 256;
const tileCache = new Map<string, Float32Array | null>();

async function loadTile(z: number, x: number, y: number): Promise<Float32Array | null> {
  const key = `${z}/${x}/${y}`;
  if (tileCache.has(key)) return tileCache.get(key)!;
  let out: Float32Array | null = null;
  try {
    const buf = await httpBinary(`https://s3.amazonaws.com/elevation-tiles-prod/terrarium/${key}.png`);
    const bmp = await createImageBitmap(new Blob([buf], { type: 'image/png' }));
    const c = document.createElement('canvas');
    c.width = c.height = TILE;
    const g = c.getContext('2d', { willReadFrequently: true })!;
    g.drawImage(bmp, 0, 0);
    const d = g.getImageData(0, 0, TILE, TILE).data;
    out = new Float32Array(TILE * TILE);
    for (let i = 0; i < TILE * TILE; i++) out[i] = d[i * 4] * 256 + d[i * 4 + 1] + d[i * 4 + 2] / 256 - 32768;
  } catch { out = null; }
  tileCache.set(key, out);
  return out;
}

export interface TerrainPlan { size: number; cell: number; zoom: number }

/** Grid resolution for a given half-size (m): at most ~513 vertices per side. */
export function planTerrain(radius: number, lat: number): TerrainPlan {
  const extent = radius * 2 * 1.08;
  const cell = Math.min(16, Math.max(2, Math.ceil((extent / 512) * 2) / 2));
  let size = Math.floor(extent / cell) + 1;
  if (size % 2 === 0) size++;
  const ground = (z: number) => (156543.03 * Math.cos((lat * Math.PI) / 180)) / 2 ** z;
  let zoom = 15;
  for (let z = 10; z <= 15; z++) if (ground(z) <= cell * 1.5) { zoom = z; break; }
  return { size, cell, zoom };
}

export interface ElevationResult { terrain: TerrainData; baseElevation: number }

/**
 * Builds a terrain around the geo origin. Heights are relative to the elevation at
 * the origin (Y = 0 there); the absolute value is returned as baseElevation.
 */
export async function importElevation(geo: GeoRef, radius: number, onStatus: (s: string) => void): Promise<ElevationResult> {
  const plan = planTerrain(radius, geo.lat0);
  const terrain = new TerrainData(plan.size, plan.cell);
  const n = plan.size;
  const world = 256 * 2 ** plan.zoom;
  // tiles needed
  const gx = (x: number, z: number) => {
    const [lat, lon] = toLatLon(geo, x, z);
    return [lonToMx(lon) * world, latToMy(lat) * world] as const;
  };
  const half = ((n - 1) * plan.cell) / 2;
  const corners = [gx(-half, -half), gx(half, -half), gx(-half, half), gx(half, half)];
  const tx0 = Math.floor(Math.min(...corners.map((c) => c[0])) / TILE) - 0, tx1 = Math.floor(Math.max(...corners.map((c) => c[0])) / TILE);
  const ty0 = Math.floor(Math.min(...corners.map((c) => c[1])) / TILE), ty1 = Math.floor(Math.max(...corners.map((c) => c[1])) / TILE);
  const tiles = new Map<string, Float32Array | null>();
  const jobs: Promise<void>[] = [];
  let done = 0;
  const total = (tx1 - tx0 + 1) * (ty1 - ty0 + 1);
  for (let ty = ty0; ty <= ty1; ty++) for (let tx = tx0; tx <= tx1; tx++) {
    jobs.push(loadTile(plan.zoom, tx, ty).then((t) => { tiles.set(`${tx},${ty}`, t); onStatus(`Loading elevation tiles ${++done}/${total}…`); }));
  }
  await Promise.all(jobs);
  if ([...tiles.values()].every((t) => t === null)) throw new Error('Elevation data (AWS Terrain Tiles) is not reachable.');

  const px = (ix: number, iy: number) => {
    const t = tiles.get(`${Math.floor(ix / TILE)},${Math.floor(iy / TILE)}`);
    if (!t) return NaN;
    return t[(((iy % TILE) + TILE) % TILE) * TILE + (((ix % TILE) + TILE) % TILE)];
  };
  const sample = (x: number, z: number) => {
    const [ax, ay] = gx(x, z);
    const fx = ax - 0.5, fy = ay - 0.5;
    const x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
    const a = px(x0, y0), b = px(x0 + 1, y0), c = px(x0, y0 + 1), d = px(x0 + 1, y0 + 1);
    const vs = [a, b, c, d];
    const ok = vs.filter((v) => !Number.isNaN(v));
    if (!ok.length) return NaN;
    const fill = ok.reduce((s, v) => s + v, 0) / ok.length;
    const [A, B, C, D] = vs.map((v) => (Number.isNaN(v) ? fill : v));
    return (A * (1 - tx) + B * tx) * (1 - ty) + (C * (1 - tx) + D * tx) * ty;
  };

  onStatus('Building terrain…');
  const base = Math.round(sample(0, 0) * 10) / 10;
  const baseElevation = Number.isNaN(base) ? 0 : base;
  const H = terrain.heights, M = terrain.materials;
  for (let iz = 0; iz < n; iz++) {
    for (let ix = 0; ix < n; ix++) {
      const e = sample(terrain.worldX(ix), terrain.worldZ(iz));
      H[iz * n + ix] = Number.isNaN(e) ? 0 : Math.round((e - baseElevation) * 100) / 100;
      M[iz * n + ix] = 1;
    }
  }
  // steep slopes become rock
  const slope = (ix: number, iz: number) => {
    const hx = (H[iz * n + Math.min(n - 1, ix + 1)] - H[iz * n + Math.max(0, ix - 1)]) / (2 * plan.cell);
    const hz = (H[Math.min(n - 1, iz + 1) * n + ix] - H[Math.max(0, iz - 1) * n + ix]) / (2 * plan.cell);
    return Math.hypot(hx, hz);
  };
  for (let iz = 0; iz < n; iz++) for (let ix = 0; ix < n; ix++) if (slope(ix, iz) > 0.75) M[iz * n + ix] = 4;
  return { terrain, baseElevation };
}
