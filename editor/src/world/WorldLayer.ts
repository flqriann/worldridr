// Merged, chunked rendering for the world objects of a worldridr map
// (Building, Road, GameMesh). Thousands of objects cost a few dozen draw calls:
// every spatial chunk (192 m) is one vertex-colored mesh that is rebuilt only
// when one of its objects changes. Picking maps the hit triangle back to the object.

import * as THREE from 'three';
import type { Instance } from '../core/Instance';
import type { Vec3 } from '../core/types';
import { buildBuilding, buildRoadRibbon, hexRgb, parseNumberList, roadTypeIndex, triangulate, type MeshArrays } from './shapes';

export const LAYER_CLASSES = new Set(['Building', 'Road', 'GameMesh']);
export const isLayerClass = (name: string) => LAYER_CLASSES.has(name);

const CHUNK = 192;
const DEG = Math.PI / 180;
const ONE = new THREE.Vector3(1, 1, 1);
const _e = new THREE.Euler();
const _q = new THREE.Quaternion();

interface Item { chunk: string; arrays: MeshArrays | null; sig: string; local?: { sig: string; arrays: MeshArrays } }
interface Chunk { key: string; mesh: THREE.Mesh | null; items: Set<Instance>; dirty: boolean; order: Instance[]; starts: number[] }

export interface LayerHit { inst: Instance; point: THREE.Vector3; normal: THREE.Vector3; distance: number }

function matrixOf(p: Vec3, o: Vec3, out = new THREE.Matrix4()) {
  _e.set(o.x * DEG, o.y * DEG, o.z * DEG, 'YXZ');
  _q.setFromEuler(_e);
  return out.compose(new THREE.Vector3(p.x, p.y, p.z), _q, ONE);
}

function transform(src: MeshArrays, m: THREE.Matrix4): MeshArrays {
  const n = src.pos.length / 3;
  const pos = new Float32Array(src.pos.length), nrm = new Float32Array(src.nrm.length);
  const e = m.elements;
  for (let i = 0; i < n; i++) {
    const x = src.pos[i * 3], y = src.pos[i * 3 + 1], z = src.pos[i * 3 + 2];
    pos[i * 3] = e[0] * x + e[4] * y + e[8] * z + e[12];
    pos[i * 3 + 1] = e[1] * x + e[5] * y + e[9] * z + e[13];
    pos[i * 3 + 2] = e[2] * x + e[6] * y + e[10] * z + e[14];
    const nx = src.nrm[i * 3], ny = src.nrm[i * 3 + 1], nz = src.nrm[i * 3 + 2];
    nrm[i * 3] = e[0] * nx + e[4] * ny + e[8] * nz;
    nrm[i * 3 + 1] = e[1] * nx + e[5] * ny + e[9] * nz;
    nrm[i * 3 + 2] = e[2] * nx + e[6] * ny + e[10] * nz;
  }
  return { pos, nrm, col: src.col };
}

export class WorldLayer {
  readonly group = new THREE.Group();
  private items = new Map<Instance, Item>();
  private chunks = new Map<string, Chunk>();
  private mats: Record<string, THREE.Material>;
  private roadsDirty = false;
  private roadsAt = 0;
  /** Terrain height lookup (set by the viewport). */
  heightAt: (x: number, z: number) => number = () => 0;

  constructor(root: THREE.Object3D) {
    this.group.name = 'world-layer';
    root.add(this.group);
    this.mats = {
      Building: new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.92, metalness: 0 }),
      GameMesh: new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.85, metalness: 0, side: THREE.DoubleSide }),
      Road: new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2 }),
    };
  }

  get count() { return this.items.size; }
  get chunkCount() { return this.chunks.size; }

  has(inst: Instance) { return this.items.has(inst); }

  /** (Re)bakes an instance after creation or any property change. */
  update(inst: Instance) {
    if (inst.props.Visible === false) { this.remove(inst); return; }
    const p = inst.props;
    const m = matrixOf(p.Position as Vec3, p.Orientation as Vec3);
    const size = p.Size as Vec3;
    let arrays: MeshArrays | null = null;
    const prev = this.items.get(inst);
    let local = prev?.local;
    if (inst.className === 'Building') {
      const sig = [p.Kind, p.Footprint, size.x, size.y, size.z, p.RoofShape, p.RoofHeight, p.Color, p.RoofColor].join('|');
      if (!local || local.sig !== sig) {
        local = { sig, arrays: buildBuilding({
          footprint: parseNumberList(p.Footprint as string), size: [size.x, size.y, size.z], kind: p.Kind as string,
          roofShape: p.RoofShape as string, roofHeight: p.RoofHeight as number, wall: p.Color as string, roof: p.RoofColor as string,
        }) };
      }
      arrays = transform(local.arrays, m);
    } else if (inst.className === 'GameMesh') {
      const sig = [p.MeshData, size.x, size.y, size.z].join('|');
      if (!local || local.sig !== sig) local = { sig, arrays: bakeGameMesh(p.MeshData as string, size) };
      arrays = transform(local.arrays, m);
    } else if (inst.className === 'Road') {
      arrays = this.bakeRoad(inst, m);
    }
    const key = `${inst.className === 'Road' ? 'R' : inst.className === 'GameMesh' ? 'M' : 'B'}${Math.floor(p.Position ? (p.Position as Vec3).x / CHUNK : 0)},${Math.floor((p.Position as Vec3).z / CHUNK)}`;
    if (prev && prev.chunk !== key) this.chunks.get(prev.chunk)?.items.delete(inst), this.touch(prev.chunk);
    this.items.set(inst, { chunk: key, arrays, sig: '', local });
    let c = this.chunks.get(key);
    if (!c) { c = { key, mesh: null, items: new Set(), dirty: true, order: [], starts: [] }; this.chunks.set(key, c); }
    c.items.add(inst);
    c.dirty = true;
  }

  private touch(key: string) { const c = this.chunks.get(key); if (c) c.dirty = true; }

  remove(inst: Instance) {
    const it = this.items.get(inst);
    if (!it) return;
    this.items.delete(inst);
    const c = this.chunks.get(it.chunk);
    if (c) { c.items.delete(inst); c.dirty = true; }
  }

  /** Terrain changed: roads follow the ground, so re-drape them (throttled in flush). */
  markRoadsDirty() { this.roadsDirty = true; }

  private bakeRoad(inst: Instance, m: THREE.Matrix4): MeshArrays {
    const pts = parseNumberList(inst.props.Points as string);
    const out: number[] = [];
    const v = new THREE.Vector3();
    const world: [number, number, number][] = [];
    for (let i = 0; i + 2 < pts.length; i += 3) {
      v.set(pts[i], 0, pts[i + 1]).applyMatrix4(m);
      world.push([v.x, v.z, pts[i + 2]]);
    }
    for (let i = 0; i < world.length; i++) {
      const a = world[i];
      if (i > 0) {
        const b = world[i - 1];
        const L = Math.hypot(a[0] - b[0], a[1] - b[1]);
        const steps = Math.min(40, Math.floor(L / 5));
        for (let s = 1; s <= steps; s++) {
          const t = s / (steps + 1);
          const x = b[0] + (a[0] - b[0]) * t, z = b[1] + (a[1] - b[1]) * t;
          out.push(x, this.heightAt(x, z) + b[2] + (a[2] - b[2]) * t, z);
        }
      }
      out.push(a[0], this.heightAt(a[0], a[1]) + a[2], a[1]);
    }
    return buildRoadRibbon(out, roadTypeIndex(inst.props.RoadType as string));
  }

  /** Rebuilds dirty chunk meshes; call once per frame. */
  flush(now = performance.now()) {
    if (this.roadsDirty && now - this.roadsAt > 250) {
      this.roadsDirty = false;
      this.roadsAt = now;
      for (const [inst, it] of this.items) if (inst.className === 'Road') { this.update(inst); void it; }
    }
    for (const c of this.chunks.values()) {
      if (!c.dirty) continue;
      c.dirty = false;
      c.order = [];
      c.starts = [];
      let total = 0;
      for (const inst of c.items) {
        const a = this.items.get(inst)?.arrays;
        if (a && a.pos.length) { c.order.push(inst); c.starts.push(total / 9); total += a.pos.length; }
      }
      if (!c.order.length) {
        if (c.mesh) { c.mesh.removeFromParent(); c.mesh.geometry.dispose(); c.mesh = null; }
        if (!c.items.size) this.chunks.delete(c.key);
        continue;
      }
      const pos = new Float32Array(total), nrm = new Float32Array(total), col = new Float32Array(total);
      let o = 0;
      for (const inst of c.order) {
        const a = this.items.get(inst)!.arrays!;
        pos.set(a.pos, o); nrm.set(a.nrm, o); col.set(a.col, o);
        o += a.pos.length;
      }
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
      g.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
      g.setAttribute('color', new THREE.BufferAttribute(col, 3));
      g.computeBoundingSphere();
      g.computeBoundingBox();
      if (!c.mesh) {
        const kind = c.key[0] === 'R' ? 'Road' : c.key[0] === 'M' ? 'GameMesh' : 'Building';
        c.mesh = new THREE.Mesh(g, this.mats[kind]);
        c.mesh.castShadow = kind !== 'Road';
        c.mesh.receiveShadow = true;
        c.mesh.userData.worldChunk = c;
        c.mesh.matrixAutoUpdate = false;
        c.mesh.updateMatrix();
        this.group.add(c.mesh);
      } else {
        c.mesh.geometry.dispose();
        c.mesh.geometry = g;
      }
    }
  }

  /** Nearest hit of a ray against all world objects. */
  raycast(rc: THREE.Raycaster): LayerHit | null {
    let best: LayerHit | null = null;
    const hits: THREE.Intersection[] = [];
    for (const c of this.chunks.values()) {
      if (!c.mesh || !c.mesh.visible) continue;
      hits.length = 0;
      c.mesh.raycast(rc, hits);
      for (const h of hits) {
        if (best && h.distance >= best.distance) continue;
        const fi = h.faceIndex ?? 0;
        let lo = 0, hi = c.starts.length - 1;
        while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (c.starts[mid] <= fi) lo = mid; else hi = mid - 1; }
        const inst = c.order[lo];
        if (!inst) continue;
        best = { inst, point: h.point.clone(), normal: h.face ? h.face.normal.clone() : new THREE.Vector3(0, 1, 0), distance: h.distance };
      }
    }
    return best;
  }

  clear() {
    for (const c of this.chunks.values()) { if (c.mesh) { c.mesh.removeFromParent(); c.mesh.geometry.dispose(); } }
    this.chunks.clear();
    this.items.clear();
  }

  dispose() {
    this.clear();
    this.group.removeFromParent();
    for (const m of Object.values(this.mats)) m.dispose();
  }
}

/** Normalized game mesh ({v: flat x,y,z in -0.5..0.5, f: [{i, c}]}) -> triangles scaled by size. */
function bakeGameMesh(text: string, size: Vec3): MeshArrays {
  const pos: number[] = [], nrm: number[] = [], col: number[] = [];
  try {
    const d = JSON.parse(text) as { v: number[]; f: { i: number[]; c: number[] }[] };
    const V = (i: number) => [d.v[i * 3] * size.x, d.v[i * 3 + 1] * size.y, d.v[i * 3 + 2] * size.z];
    for (const f of d.f) {
      const rgb = hexRgb('#' + f.c.map((x) => Math.round(Math.max(0, Math.min(255, x))).toString(16).padStart(2, '0')).join(''));
      // fan for convex faces, ear clipping for the rest (projected on the dominant axis)
      const idx = f.i;
      let tris: number[][] = [];
      if (idx.length === 3) tris = [[0, 1, 2]];
      else if (idx.length === 4) tris = [[0, 1, 2], [0, 2, 3]];
      else {
        const p0 = V(idx[0]), p1 = V(idx[1]), p2 = V(idx[2]);
        const nx = (p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]);
        const ny = (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]);
        const nz = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
        const flat: number[] = [];
        for (const i of idx) { const p = V(i); if (Math.abs(ny) >= Math.abs(nx) && Math.abs(ny) >= Math.abs(nz)) flat.push(p[0], p[2]); else if (Math.abs(nx) >= Math.abs(nz)) flat.push(p[1], p[2]); else flat.push(p[0], p[1]); }
        tris = triangulate(flat);
      }
      for (const t of tris) {
        const a = V(idx[t[0]]), b = V(idx[t[1]]), c = V(idx[t[2]]);
        const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const l = Math.hypot(nx, ny, nz) || 1;
        nx /= l; ny /= l; nz /= l;
        for (const p of [a, b, c]) { pos.push(p[0], p[1], p[2]); nrm.push(nx, ny, nz); col.push(rgb[0], rgb[1], rgb[2]); }
      }
    }
  } catch { /* broken mesh: draw nothing */ }
  return { pos: new Float32Array(pos), nrm: new Float32Array(nrm), col: new Float32Array(col) };
}
