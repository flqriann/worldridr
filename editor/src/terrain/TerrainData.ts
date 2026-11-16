// Heightmap terrain backed by the Rust core (engine/src/terrain.rs).
// Heights / materials / water live in WASM memory; this class owns the
// native handle and exposes typed views plus high-level operations.

import { base64ToBytes, bytesToBase64 } from '../core/encoding';
import { engine, view } from '../engine/engine';

export const TERRAIN_MATERIALS = [
  { id: 0, name: 'Air', color: '#000000' },
  { id: 1, name: 'Grass', color: '#5a8f3a' },
  { id: 2, name: 'Dirt', color: '#7a5636' },
  { id: 3, name: 'Sand', color: '#d8c48a' },
  { id: 4, name: 'Rock', color: '#7d7b78' },
  { id: 5, name: 'Snow', color: '#eef3f7' },
  { id: 6, name: 'Mud', color: '#4e3b2a' },
  { id: 7, name: 'Ice', color: '#a9d4ee' },
] as const;
export const terrainMaterialId = (name: string) => TERRAIN_MATERIALS.find((m) => m.name === name)?.id ?? 1;

export interface Region { x0: number; z0: number; x1: number; z1: number }
/** Copy of (part of) the terrain for undo. `region` absent = whole terrain. */
export interface TerrainSnapshot { region?: Region; heights: Float32Array; materials: Uint8Array; water: Uint8Array }

export const DEFAULT_TERRAIN_SIZE = 257; // vertices per side -> 1024 studs at 4 studs/cell
export const CHUNK_CELLS = 32;

const TOOL_IDS = { raise: 0, lower: 1, smooth: 2, flatten: 3, paint: 4, addWater: 5, removeWater: 6, erase: 7 } as const;
const PRESET_IDS = { flat: 0, hills: 1, mountains: 2, valleys: 3, island: 4 } as const;
export type BrushToolId = keyof typeof TOOL_IDS;
export type PresetId = keyof typeof PRESET_IDS;

const registry = new FinalizationRegistry<number>((h) => { try { engine().terrain_free(h); } catch { /* engine gone */ } });

export class TerrainData {
  readonly size: number;
  readonly cell: number;
  private handle: number;

  constructor(size = DEFAULT_TERRAIN_SIZE, cell = 4) {
    this.handle = engine().terrain_new(size, cell);
    this.size = engine().terrain_size(this.handle);
    this.cell = cell;
    registry.register(this, this.handle);
  }

  private get n() { return this.size * this.size; }
  /** Live views into WASM memory – do not keep them across engine calls that may allocate. */
  get heights(): Float32Array { return view.f32(engine().terrain_heights(this.handle), this.n); }
  get materials(): Uint8Array { return view.u8(engine().terrain_materials(this.handle), this.n); }
  get water(): Uint8Array { return view.u8(engine().terrain_water(this.handle), this.n); }

  get origin() { return -((this.size - 1) * this.cell) / 2; }
  get extent() { return (this.size - 1) * this.cell; }
  idx(ix: number, iz: number) { return iz * this.size + ix; }
  inBounds(ix: number, iz: number) { return ix >= 0 && iz >= 0 && ix < this.size && iz < this.size; }
  worldX(ix: number) { return this.origin + ix * this.cell; }
  worldZ(iz: number) { return this.origin + iz * this.cell; }
  gridX(x: number) { return (x - this.origin) / this.cell; }
  gridZ(z: number) { return (z - this.origin) / this.cell; }
  get chunksPerSide() { return Math.ceil((this.size - 1) / CHUNK_CELLS); }

  hasSolid(): boolean { return engine().terrain_has_solid(this.handle) === 1; }
  hasWater(): boolean { return this.water.some((w) => w !== 0); }

  heightAt(x: number, z: number): number | undefined {
    const h = engine().terrain_height_at(this.handle, x, z);
    return Number.isNaN(h) ? undefined : h;
  }

  waterAt(x: number, z: number): boolean {
    const ix = Math.round(this.gridX(x)), iz = Math.round(this.gridZ(z));
    return this.inBounds(ix, iz) && this.water[this.idx(ix, iz)] === 1;
  }

  normalAt(x: number, z: number): [number, number, number] {
    const e = engine();
    const p = e.forge_alloc(12);
    e.terrain_normal_at(this.handle, x, z, p);
    const out = Array.from(view.f32(p, 3)) as [number, number, number];
    e.forge_free(p, 12);
    return out;
  }

  /** Distance to the surface along a ray, or null. */
  raycast(ox: number, oy: number, oz: number, dx: number, dy: number, dz: number, max = 5000): number | null {
    const d = engine().terrain_raycast(this.handle, ox, oy, oz, dx, dy, dz, max);
    return d < 0 ? null : d;
  }

  /** One brush step; returns the changed vertex region or null. */
  brush(tool: BrushToolId, x: number, z: number, radius: number, strength: number, material: number, flattenHeight: number, dt: number): Region | null {
    const e = engine();
    if (!e.terrain_brush(this.handle, TOOL_IDS[tool], x, z, radius, strength, material, flattenHeight, dt)) return null;
    const r = view.i32(e.terrain_dirty(this.handle), 4);
    return { x0: r[0], z0: r[1], x1: r[2], z1: r[3] };
  }

  generate(o: { preset: PresetId; seed: number; amplitude: number; scale: number; baseHeight: number; water: boolean; waterLevel: number }) {
    engine().terrain_generate(this.handle, PRESET_IDS[o.preset], o.seed >>> 0, o.amplitude, o.scale, o.baseHeight, o.water ? 1 : 0, o.waterLevel);
  }

  clear() { engine().terrain_clear(this.handle); }

  /** Mesh data of one chunk (copied out of WASM memory) or null if empty. */
  meshChunk(cx: number, cz: number, decoration: boolean): { positions: Float32Array; normals: Float32Array; colors: Float32Array; indices: Uint32Array } | null {
    const e = engine();
    const count = e.terrain_mesh_chunk(this.handle, cx, cz, CHUNK_CELLS, decoration ? 1 : 0);
    if (!count) return null;
    const plen = e.terrain_mesh_positions_len(this.handle);
    return {
      positions: view.f32(e.terrain_mesh_positions(this.handle), plen).slice(),
      normals: view.f32(e.terrain_mesh_normals(this.handle), plen).slice(),
      colors: view.f32(e.terrain_mesh_colors(this.handle), plen).slice(),
      indices: view.u32(e.terrain_mesh_indices(this.handle), count).slice(),
    };
  }

  waterMesh(level: number): Float32Array {
    const e = engine();
    const verts = e.terrain_mesh_water(this.handle, level);
    return view.f32(e.terrain_mesh_positions(this.handle), verts * 3).slice();
  }

  // ---- snapshots (undo) ---------------------------------------------------------

  snapshot(region?: Region): TerrainSnapshot {
    if (!region) return { heights: this.heights.slice(), materials: this.materials.slice(), water: this.water.slice() };
    const w = region.x1 - region.x0 + 1, h = region.z1 - region.z0 + 1;
    const hs = new Float32Array(w * h), ms = new Uint8Array(w * h), ws = new Uint8Array(w * h);
    const H = this.heights, M = this.materials, W = this.water;
    for (let z = 0; z < h; z++) {
      const src = this.idx(region.x0, region.z0 + z);
      hs.set(H.subarray(src, src + w), z * w);
      ms.set(M.subarray(src, src + w), z * w);
      ws.set(W.subarray(src, src + w), z * w);
    }
    return { region: { ...region }, heights: hs, materials: ms, water: ws };
  }

  restore(s: TerrainSnapshot) {
    const H = this.heights, M = this.materials, W = this.water;
    if (!s.region) { H.set(s.heights); M.set(s.materials); W.set(s.water); return; }
    const r = s.region;
    const w = r.x1 - r.x0 + 1, h = r.z1 - r.z0 + 1;
    for (let z = 0; z < h; z++) {
      const dst = this.idx(r.x0, r.z0 + z);
      H.set(s.heights.subarray(z * w, z * w + w), dst);
      M.set(s.materials.subarray(z * w, z * w + w), dst);
      W.set(s.water.subarray(z * w, z * w + w), dst);
    }
  }

  clone(): TerrainData {
    const t = new TerrainData(this.size, this.cell);
    t.restore(this.snapshot());
    return t;
  }

  // ---- serialization ------------------------------------------------------------

  serialize(): { size: number; cell: number; empty?: boolean; heights?: string; materials?: string; water?: string } {
    if (!this.hasSolid() && !this.hasWater()) return { size: this.size, cell: this.cell, empty: true };
    return {
      size: this.size,
      cell: this.cell,
      heights: bytesToBase64(new Uint8Array(this.heights.slice().buffer)),
      materials: bytesToBase64(this.materials.slice()),
      water: bytesToBase64(this.water.slice()),
    };
  }

  static deserialize(d: { size: number; cell: number; empty?: boolean; heights?: string; materials?: string; water?: string }): TerrainData {
    if (!Number.isInteger(d?.size) || d.size < 2 || d.size > 4097 || !(d.cell > 0)) throw new Error('Terrain header is invalid');
    const t = new TerrainData(d.size, d.cell);
    if (d.empty) return t;
    const expected = d.size * d.size;
    const hb = base64ToBytes(d.heights ?? '');
    if (hb.byteLength !== expected * 4) throw new Error('Terrain height data is corrupt');
    const mb = base64ToBytes(d.materials ?? ''), wb = base64ToBytes(d.water ?? '');
    if (mb.length !== expected || wb.length !== expected) throw new Error('Terrain material data is corrupt');
    t.heights.set(new Float32Array(hb.buffer.slice(hb.byteOffset, hb.byteOffset + hb.byteLength)));
    t.materials.set(mb);
    t.water.set(wb);
    return t;
  }
}

export function unionRegion(a: Region | null, b: Region): Region {
  if (!a) return { ...b };
  return { x0: Math.min(a.x0, b.x0), z0: Math.min(a.z0, b.z0), x1: Math.max(a.x1, b.x1), z1: Math.max(a.z1, b.z1) };
}
