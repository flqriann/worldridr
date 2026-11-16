// worldridr-map v1 (git-friendly): worldridr-map.json + tiles/z15_x_y.json (one object/road per line,
// sorted by id, rounded) + optional terrain.json + scripts/. Mirrors src/mapformat.cpp of the game.
//
// The base world is OpenStreetMap, which the game streams by itself. A map repository therefore holds
// only what people ADD or CHANGE; imported OSM objects are skipped unless `includeOsm` is set.

import * as THREE from 'three';
import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import type { Document } from '../editor/Document';
import { createFromTemplate } from '../project/templates';
import { TerrainData } from '../terrain/TerrainData';
import { orientationToQuat, quatToOrientation } from '../render/geometry';
import { latToMy, lonToMx, makeGeo, myToLat, tileOf, toLatLon, toLocal, type GeoRef } from './geo';
import { buildBuilding, parseNumberList, ROAD_TYPES, roadTypeIndex } from './shapes';

export const TILE_ZOOM = 15;
export type MapFiles = Record<string, string>;
export interface ExportOptions { includeOsm: boolean; terrain: boolean; scripts: boolean }
export interface ExportReport { objects: number; roads: number; meshes: number; scripts: number; skippedOsm: number; warnings: string[] }

const rnd = (v: number, d: number) => { const m = 10 ** d; return Math.round(v * m) / m; };
const rgb255 = (hex: string): number[] => {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  return m ? [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)] : [200, 200, 200];
};
const _c = new THREE.Color();
/** Linear vertex color (as stored in the render buffers) -> 0..255 sRGB as used by the game. */
const linearTo255 = (r: number, g: number, b: number): number[] => {
  _c.setRGB(Math.max(0, Math.min(1, r)), Math.max(0, Math.min(1, g)), Math.max(0, Math.min(1, b)), THREE.LinearSRGBColorSpace).convertLinearToSRGB();
  return [Math.round(_c.r * 255), Math.round(_c.g * 255), Math.round(_c.b * 255)];
};
const hexOf = (c: number[]) => '#' + c.slice(0, 3).map((x) => Math.max(0, Math.min(255, Math.round(x))).toString(16).padStart(2, '0')).join('');
const safeJson = (s: unknown): Record<string, unknown> => { try { const o = JSON.parse(String(s || '{}')); return o && typeof o === 'object' ? o : {}; } catch { return {}; } };

export function geoOf(doc: Document): { geo: GeoRef; base: number } {
  const p = doc.dm.workspace.props;
  return { geo: makeGeo(p.GeoLat as number, p.GeoLon as number), base: p.GeoElevation as number };
}

const _q = new THREE.Quaternion();
const RZ90 = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 0, 1), Math.PI / 2);
const RZM90 = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 0, 1), -Math.PI / 2);

/** Game rotation [yaw, pitch, roll] in degrees from a Forge orientation (optionally post-rotated). */
function gameRot(o: Vec3, post?: THREE.Quaternion): number[] {
  _q.copy(orientationToQuat(o));
  if (post) _q.multiply(post);
  const e = quatToOrientation(_q); // (x = pitch, y = yaw, z = roll) in YXZ order = game's Ry*Rx*Rz
  return [e.y, e.x, e.z];
}

interface Tile { objects: { id: string; line: string }[]; roads: { id: string; line: string }[] }

export function exportMap(doc: Document, opt: ExportOptions): { files: MapFiles; report: ExportReport } {
  const { geo, base } = geoOf(doc);
  const report: ExportReport = { objects: 0, roads: 0, meshes: 0, scripts: 0, skippedOsm: 0, warnings: [] };
  const tiles = new Map<string, Tile>();
  const tileFor = (lat: number, lon: number) => {
    const [x, y] = tileOf(lat, lon, TILE_ZOOM);
    const k = `z${TILE_ZOOM}_x${x}_y${y}`;
    let t = tiles.get(k);
    if (!t) { t = { objects: [], roads: [] }; tiles.set(k, t); }
    return t;
  };
  const files: MapFiles = {};
  const scriptIndex: { path: string; name: string; kind: string; target: string }[] = [];

  doc.dm.workspace.walk((i) => {
    if (i.className === 'Terrain') return;
    if (i.isA('LuaSourceContainer')) return;
    if (!i.isA('BasePart')) return;
    const p = i.props;
    const osm = !!(p.OsmId as string);
    if (osm && !opt.includeOsm) { report.skippedOsm++; return; }
    const id = (p.MapId as string) || i.id;
    const pos = p.Position as Vec3, size = p.Size as Vec3;
    const [lat, lon] = toLatLon(geo, pos.x, pos.z);
    const extra = safeJson(p.MapExtra);

    if (i.className === 'Road') {
      const pts = parseNumberList(p.Points as string);
      const q = orientationToQuat(p.Orientation as Vec3);
      const out: number[][] = [];
      const v = new THREE.Vector3();
      for (let k = 0; k + 2 < pts.length; k += 3) {
        v.set(pts[k], 0, pts[k + 1]).applyQuaternion(q);
        const [la, lo] = toLatLon(geo, v.x + pos.x, v.z + pos.z);
        out.push([rnd(la, 8), rnd(lo, 8), rnd(Math.max(0, pts[k + 2]), 3)]);
      }
      if (out.length < 2) return;
      const r: Record<string, unknown> = { id, kind: roadTypeIndex(p.RoadType as string), points: out, ...extra };
      if (p.StreetName) r.street = p.StreetName;
      if (p.OsmId) r.osm = p.OsmId;
      tileFor(out[0][0], out[0][1]).roads.push({ id, line: JSON.stringify(r) });
      report.roads++;
      return;
    }

    const o: Record<string, unknown> = { id, name: p.Name };
    let shape = 'block', rot = gameRot(p.Orientation as Vec3), scale = [size.x, size.y, size.z];
    let mesh: unknown;
    const color = rgb255(p.Color as string);
    if (i.className === 'Building') {
      const spec = {
        footprint: parseNumberList(p.Footprint as string), size: [size.x, size.y, size.z] as [number, number, number], kind: p.Kind as string,
        roofShape: p.RoofShape as string, roofHeight: p.RoofHeight as number, wall: p.Color as string, roof: p.RoofColor as string,
      };
      mesh = meshFromArrays(buildBuilding(spec));
      shape = 'mesh'; scale = [1, 1, 1];
      o.building = { kind: p.Kind, fp: spec.footprint, size: scale && [rnd(size.x, 3), rnd(size.y, 3), rnd(size.z, 3)], roofShape: p.RoofShape, roofHeight: p.RoofHeight, roof: p.RoofColor, levels: p.Levels };
      report.meshes++;
    } else if (i.className === 'GameMesh') {
      const d = safeJson(p.MeshData) as { v?: number[]; f?: unknown[] };
      const v: number[][] = [];
      for (let k = 0; k + 2 < (d.v?.length ?? 0); k += 3) v.push([rnd(d.v![k] * size.x, 4), rnd(d.v![k + 1] * size.y, 4), rnd(d.v![k + 2] * size.z, 4)]);
      mesh = { v, f: d.f ?? [] };
      shape = 'mesh'; scale = [1, 1, 1];
      report.meshes++;
    } else if (i.className === 'MeshPart') {
      report.warnings.push(`${i.getFullName()}: MeshPart (glb) is not part of the map format and was exported as a block`);
    } else if (i.className === 'Part' || i.className === 'SpawnLocation') {
      const sh = p.Shape as string;
      if (sh === 'Ball') shape = 'sphere';
      else if (sh === 'Cylinder') { shape = 'cylinder'; rot = gameRot(p.Orientation as Vec3, RZM90); scale = [size.y, size.x, size.z]; }
      if (i.className === 'SpawnLocation') o.spawn = true;
    } else if (i.className === 'WedgePart') shape = 'wedge';

    o.shape = shape;
    o.lat = rnd(lat, 8); o.lon = rnd(lon, 8);
    o.alt = rnd(pos.y + base, 4);
    o.rot = rot.map((x) => rnd(x, 5));
    o.scale = scale.map((x) => rnd(x, 4));
    o.color = color;
    if (!p.CanCollide) o.solid = false;
    if (mesh) o.mesh = mesh;
    for (const [k, v] of Object.entries(extra)) if (!(k in o)) o[k] = v;
    if (p.OsmId) o.osm = p.OsmId;
    // scripts attached to this object
    if (opt.scripts) {
      const names: string[] = [];
      for (const c of i.children) {
        if (!c.isA('LuaSourceContainer')) continue;
        const path = `scripts/${slug(i.name)}_${id.slice(0, 6)}_${slug(c.name)}.lua`;
        files[path] = `-- ${c.className} "${c.name}" on object "${i.name}" (id ${id}), exported from WorldRidr Studio\n` + (c.props.Source as string);
        names.push(path);
        scriptIndex.push({ path, name: c.name, kind: c.className, target: id });
        report.scripts++;
      }
      if (names.length) o.scripts = names;
    }
    tileFor(lat, lon).objects.push({ id, line: JSON.stringify(o) });
    report.objects++;
  });

  if (opt.scripts) {
    // scripts that are not attached to an object (services, folders)
    for (const s of doc.dm.getDescendants()) {
      if (!s.isA('LuaSourceContainer') || s.parent?.isA('BasePart')) continue;
      const path = `scripts/${slug(s.getFullName())}.lua`;
      if (files[path]) continue;
      files[path] = `-- ${s.className} "${s.getFullName()}", exported from WorldRidr Studio\n` + (s.props.Source as string);
      scriptIndex.push({ path, name: s.name, kind: s.className, target: '' });
      report.scripts++;
    }
  }

  const cmp = (a: { id: string }, b: { id: string }) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0);
  for (const [name, t] of tiles) {
    t.objects.sort(cmp); t.roads.sort(cmp);
    const m = name.match(/^z(\d+)_x(\d+)_y(\d+)$/)!;
    let s = `{\n  "format": "worldridr-tile",\n  "version": 1,\n  "tile": {"z":${m[1]},"x":${m[2]},"y":${m[3]}},\n`;
    s += '  "objects": [' + (t.objects.length ? '\n' + t.objects.map((x) => '    ' + x.line).join(',\n') + '\n  ' : '') + '],\n';
    s += '  "roads": [' + (t.roads.length ? '\n' + t.roads.map((x) => '    ' + x.line).join(',\n') + '\n  ' : '') + ']\n}\n';
    files[`tiles/${name}.json`] = s;
  }

  const wp = doc.dm.workspace.props;
  const manifest: Record<string, unknown> = {
    format: 'worldridr-map', version: 1, name: doc.name, tile_zoom: TILE_ZOOM, base: 'openstreetmap',
    origin: { lat: wp.GeoLat, lon: wp.GeoLon, elevation: wp.GeoElevation, place: wp.PlaceName },
    palette: [], editor: 'worldridr-studio',
  };
  if (scriptIndex.length) manifest.scripts = scriptIndex;
  files['worldridr-map.json'] = JSON.stringify(manifest, null, 2) + '\n';

  if (opt.terrain) {
    const t = doc.dm.terrainInstance?.terrain;
    if (t && t.hasSolid()) { files['terrain.json'] = exportTerrain(t, geo, base); }
  }
  files['README.md'] = MAP_README;
  files['.gitattributes'] = '* text=auto eol=lf\n';
  return { files, report };
}

const slug = (s: string) => s.replace(/[^a-z0-9._-]+/gi, '_').replace(/^_+|_+$/g, '').slice(0, 60) || 'x';

function meshFromArrays(a: { pos: Float32Array; col: Float32Array }) {
  const vmap = new Map<string, number>();
  const v: number[][] = [];
  const f: { i: number[]; c: number[] }[] = [];
  const idx = (k: number) => {
    const x = rnd(a.pos[k * 3], 3), y = rnd(a.pos[k * 3 + 1], 3), z = rnd(a.pos[k * 3 + 2], 3);
    const key = `${x},${y},${z}`;
    let n = vmap.get(key);
    if (n === undefined) { n = v.length; v.push([x, y, z]); vmap.set(key, n); }
    return n;
  };
  const tri = a.pos.length / 9;
  for (let t = 0; t < tri; t++) {
    const i0 = idx(t * 3), i1 = idx(t * 3 + 1), i2 = idx(t * 3 + 2);
    if (i0 === i1 || i1 === i2 || i0 === i2) continue;
    const c = linearTo255(a.col[t * 9], a.col[t * 9 + 1], a.col[t * 9 + 2]);
    f.push({ i: [i0, i1, i2], c });
  }
  return { v, f };
}

// ---- terrain.json ---------------------------------------------------------------------

function exportTerrain(t: TerrainData, geo: GeoRef, base: number): string {
  const ground = (z: number) => (156543.03 * Math.cos((geo.lat0 * Math.PI) / 180)) / 2 ** z;
  let zoom = 20;
  for (let z = 12; z <= 20; z++) if (ground(z) <= t.cell) { zoom = z; break; }
  const world = 256 * 2 ** zoom;
  const half = t.extent / 2;
  const toPix = (x: number, z: number) => { const [la, lo] = toLatLon(geo, x, z); return [lonToMx(lo) * world, latToMy(la) * world]; };
  const cs = [toPix(-half, -half), toPix(half, -half), toPix(-half, half), toPix(half, half)];
  const px0 = Math.floor(Math.min(...cs.map((c) => c[0]))), px1 = Math.ceil(Math.max(...cs.map((c) => c[0])));
  const py0 = Math.floor(Math.min(...cs.map((c) => c[1]))), py1 = Math.ceil(Math.max(...cs.map((c) => c[1])));
  const w = px1 - px0, h = py1 - py0;
  const rows: string[] = [];
  for (let y = 0; y < h; y++) {
    const tokens: string[] = [];
    let gap = 0, have = false, last = 0, run = 0, runD = 0;
    const flushRun = () => { if (run) { tokens.push(run > 1 ? `${runD}x${run}` : `${runD}`); run = 0; } };
    const flushGap = () => { if (gap) { tokens.push(gap > 1 ? `nx${gap}` : 'n'); gap = 0; } };
    for (let x = 0; x < w; x++) {
      const gx = px0 + x + 0.5, gy = py0 + y + 0.5;
      const lon = (gx / world) * 360 - 180, lat = myToLat(gy / world);
      const [lx, lz] = toLocal(geo, lat, lon);
      const hh = t.heightAt(lx, lz);
      if (hh === undefined) { flushRun(); gap++; have = false; continue; }
      const v = Math.round((hh + base) * 10);
      flushGap();
      if (!have) { tokens.push(String(v)); have = true; last = v; run = 0; continue; }
      const d = v - last; last = v;
      if (run && d === runD) run++; else { flushRun(); run = 1; runD = d; }
    }
    flushRun(); flushGap();
    rows.push(tokens.join(' '));
  }
  return JSON.stringify({ format: 'worldridr-terrain', version: 1, zoom, px: px0, py: py0, w, h, rows }) + '\n';
}

function importTerrain(text: string, geo: GeoRef, base: number): TerrainData | null {
  const j = JSON.parse(text);
  if (j.format !== 'worldridr-terrain') return null;
  const { zoom, px, py, w, h } = j as { zoom: number; px: number; py: number; w: number; h: number };
  const grid = new Float32Array(w * h).fill(NaN);
  (j.rows as string[]).forEach((row, y) => {
    let x = 0, last = 0, have = false;
    for (const tok of row.split(' ')) {
      if (!tok) continue;
      if (tok[0] === 'n') { x += tok.includes('x') ? parseInt(tok.slice(3), 10) : 1; have = false; continue; }
      const cx = tok.indexOf('x');
      const d = parseInt(tok, 10), n = cx < 0 ? 1 : parseInt(tok.slice(cx + 1), 10);
      if (!have) { last = d; grid[y * w + x] = last / 10; x++; have = true; continue; }
      for (let k = 0; k < n && x < w; k++, x++) { last += d; grid[y * w + x] = last / 10; }
    }
  });
  const world = 256 * 2 ** zoom;
  const cell = Math.max(2, Math.min(16, Math.round(((156543.03 * Math.cos((geo.lat0 * Math.PI) / 180)) / 2 ** zoom) * 2) / 2));
  const size = Math.min(1025, (Math.floor(Math.max(w, h) * ((156543.03 * Math.cos((geo.lat0 * Math.PI) / 180)) / 2 ** zoom) / cell) | 1) + 2);
  const t = new TerrainData(size, cell);
  const H = t.heights, M = t.materials;
  const at = (ix: number, iy: number) => (ix < 0 || iy < 0 || ix >= w || iy >= h ? NaN : grid[iy * w + ix]);
  for (let iz = 0; iz < size; iz++) for (let ix = 0; ix < size; ix++) {
    const [la, lo] = toLatLon(geo, t.worldX(ix), t.worldZ(iz));
    const gx = lonToMx(lo) * world - px - 0.5, gy = latToMy(la) * world - py - 0.5;
    const x0 = Math.floor(gx), y0 = Math.floor(gy), fx = gx - x0, fy = gy - y0;
    const a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    const ok = [a, b, c, d].filter((v) => !Number.isNaN(v));
    if (!ok.length) continue;
    const m = ok.reduce((s, v) => s + v, 0) / ok.length;
    const [A, B, C, D] = [a, b, c, d].map((v) => (Number.isNaN(v) ? m : v));
    H[iz * size + ix] = (A * (1 - fx) + B * fx) * (1 - fy) + (C * (1 - fx) + D * fx) * fy - base;
    M[iz * size + ix] = 1;
  }
  return t;
}

// ---- import ------------------------------------------------------------------------------

export interface ImportReport { objects: number; roads: number; meshes: number; terrain: boolean; warnings: string[] }

/** Builds a new project from the files of a map folder. */
export function importMap(files: MapFiles, name: string): { doc: Document; report: ImportReport } {
  const mt = files['worldridr-map.json'];
  if (!mt) throw new Error('worldridr-map.json not found – this is not a worldridr map folder.');
  const man = JSON.parse(mt);
  if (man.format !== 'worldridr-map' || (man.version ?? 99) > 1) throw new Error('Unknown map format or a newer version.');
  const report: ImportReport = { objects: 0, roads: 0, meshes: 0, terrain: false, warnings: [] };
  const doc = createFromTemplate('empty');
  doc.name = man.name || name;
  const ws = doc.dm.workspace;

  type Obj = Record<string, unknown> & { id: string; shape: string; lat: number; lon: number; alt: number; rot: number[]; scale: number[]; color: number[] };
  const objs: Obj[] = [], roads: (Record<string, unknown> & { id: string; points: number[][] })[] = [];
  for (const [path, text] of Object.entries(files)) {
    if (!/^tiles\/.*\.json$/.test(path)) continue;
    try {
      const tile = JSON.parse(text);
      for (const o of tile.objects ?? []) objs.push(o);
      for (const r of tile.roads ?? []) roads.push(r);
    } catch { report.warnings.push(`${path}: unreadable (merge conflict?) – skipped`); }
  }

  let lat0 = man.origin?.lat, lon0 = man.origin?.lon;
  if (typeof lat0 !== 'number') {
    const pts = [...objs.map((o) => [o.lat, o.lon]), ...roads.map((r) => r.points[0])];
    lat0 = pts.length ? pts.reduce((s, p) => s + p[0], 0) / pts.length : 48.2082;
    lon0 = pts.length ? pts.reduce((s, p) => s + p[1], 0) / pts.length : 16.3738;
  }
  const geo = makeGeo(lat0, lon0);
  let base = typeof man.origin?.elevation === 'number' ? man.origin.elevation : NaN;
  ws.props.GeoLat = lat0; ws.props.GeoLon = lon0; ws.props.Gravity = 9.81; ws.props.PlaceName = man.origin?.place || man.name || 'Map';
  doc.dm.getService('Lighting')!.props.FogEnd = 4000;
  const sp = doc.dm.getService('StarterPlayer')!.props;
  sp.CharacterWalkSpeed = 6; sp.CharacterJumpPower = 5;

  if (files['terrain.json']) {
    try {
      if (Number.isNaN(base)) base = 0;
      const t = importTerrain(files['terrain.json'], geo, base);
      if (t) { doc.dm.terrainInstance!.terrain = t; report.terrain = true; }
    } catch (e) { report.warnings.push('terrain.json: ' + (e as Error).message); }
  }
  if (Number.isNaN(base)) base = objs.length ? Math.min(...objs.map((o) => o.alt)) : 0;
  ws.props.GeoElevation = base;

  const folders = new Map<string, Instance>();
  const into = (n: string) => {
    let f = folders.get(n);
    if (!f) { f = new Instance('Folder'); f.props.Name = n; f.setParent(ws); folders.set(n, f); }
    return f;
  };
  const known = new Set(['id', 'name', 'shape', 'lat', 'lon', 'alt', 'rot', 'scale', 'color', 'solid', 'mesh', 'building', 'scripts', 'osm', 'uid', 'spawn', 'texture', 'model', 'layer', 'tile']);

  for (const o of objs) {
    try {
      const [x, z] = toLocal(geo, o.lat, o.lon);
      const extra: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(o)) if (!known.has(k)) extra[k] = v;
      for (const k of ['texture', 'model', 'layer', 'tile']) if (o[k] !== undefined && o[k] !== '') extra[k] = o[k];
      const rot = o.rot ?? [0, 0, 0], sc = o.scale ?? [1, 1, 1];
      const ori = v3(rot[1], rot[0], rot[2]);
      const pos = v3(x, o.alt - base, z);
      const color = hexOf(o.color ?? [200, 200, 200]);
      let inst: Instance;
      if (o.shape === 'mesh' && o.mesh) {
        const m = o.mesh as { v: number[][]; f: unknown[] };
        const b = o.building as Record<string, unknown> | undefined;
        if (b && Array.isArray(b.fp)) {
          inst = new Instance('Building', undefined);
          const s = b.size as number[];
          inst.props.Size = v3(s[0], s[1], s[2]);
          inst.props.Footprint = JSON.stringify(b.fp);
          inst.props.Kind = (b.kind as string) || 'Building';
          inst.props.RoofShape = (b.roofShape as string) || 'Flat';
          inst.props.RoofHeight = (b.roofHeight as number) || 0;
          inst.props.RoofColor = (b.roof as string) || '#8a4b3a';
          inst.props.Levels = (b.levels as number) || 0;
          inst.props.Color = color;
          inst.props.Position = pos; inst.props.Orientation = ori;
        } else {
          inst = new Instance('GameMesh');
          let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
          for (const p of m.v) for (let a = 0; a < 3; a++) { lo[a] = Math.min(lo[a], p[a] * sc[a]); hi[a] = Math.max(hi[a], p[a] * sc[a]); }
          if (!m.v.length) { lo = [0, 0, 0]; hi = [1, 1, 1]; }
          const ext = [0, 1, 2].map((a) => Math.max(0.01, hi[a] - lo[a])), ctr = [0, 1, 2].map((a) => (hi[a] + lo[a]) / 2);
          const flat: number[] = [];
          for (const p of m.v) for (let a = 0; a < 3; a++) flat.push(rnd((p[a] * sc[a] - ctr[a]) / ext[a], 5));
          inst.props.MeshData = JSON.stringify({ v: flat, f: m.f });
          inst.props.Size = v3(ext[0], ext[1], ext[2]);
          // object origin -> bbox center, rotated into world
          const off = new THREE.Vector3(ctr[0], ctr[1], ctr[2]).applyQuaternion(orientationToQuat(ori));
          inst.props.Position = v3(pos.x + off.x, pos.y + off.y, pos.z + off.z);
          inst.props.Orientation = ori;
        }
        report.meshes++;
      } else {
        const sh = o.shape as string;
        inst = new Instance(o.spawn ? 'SpawnLocation' : sh === 'wedge' ? 'WedgePart' : 'Part');
        inst.props.Position = pos;
        if (sh === 'cylinder') {
          inst.props.Shape = 'Cylinder';
          inst.props.Size = v3(sc[1], sc[0], sc[2]);
          inst.props.Orientation = quatToOrientation(orientationToQuat(ori).multiply(RZ90));
        } else {
          if (sh === 'sphere') inst.props.Shape = 'Ball';
          inst.props.Size = v3(sc[0], sc[1], sc[2]);
          inst.props.Orientation = ori;
        }
        inst.props.Color = color;
        if (sh === 'model') report.warnings.push(`${o.name}: 3D model "${o.model}" is shown as a block`);
      }
      inst.props.Name = (o.name as string) || inst.className;
      inst.props.Anchored = true;
      if (o.solid === false) inst.props.CanCollide = false;
      inst.props.MapId = o.id;
      if (o.osm) inst.props.OsmId = String(o.osm);
      if (Object.keys(extra).length) inst.props.MapExtra = JSON.stringify(extra);
      inst.setParent(into(inst.className === 'Building' ? 'Buildings' : 'Objects'));
      report.objects++;
    } catch (e) { report.warnings.push(`object ${o.id}: ${(e as Error).message}`); }
  }

  for (const r of roads) {
    try {
      const w = r.points.map((p) => { const [x, z] = toLocal(geo, p[0], p[1]); return [x, z, p[2] ?? 0]; });
      if (w.length < 2) continue;
      let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
      for (const p of w) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
      const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
      const inst = new Instance('Road');
      const kind = Math.max(0, Math.min(ROAD_TYPES.length - 1, Number(r.kind ?? r.type ?? 0)));
      inst.props.RoadType = ROAD_TYPES[kind].name;
      inst.props.Points = JSON.stringify(w.flatMap((p) => [rnd(p[0] - cx, 3), rnd(p[1] - cz, 3), rnd(p[2], 3)]));
      inst.props.Position = v3(cx, doc.dm.terrainInstance?.terrain?.heightAt(cx, cz) ?? 0, cz);
      inst.props.Size = v3(Math.max(2, x1 - x0), 0.3, Math.max(2, z1 - z0));
      inst.props.Name = (r.street as string) || 'Road';
      inst.props.StreetName = (r.street as string) || '';
      inst.props.MapId = r.id;
      if (r.osm) inst.props.OsmId = String(r.osm);
      const extra: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(r)) if (!['id', 'kind', 'type', 'points', 'street', 'osm', 'uid'].includes(k)) extra[k] = v;
      if (Object.keys(extra).length) inst.props.MapExtra = JSON.stringify(extra);
      inst.setParent(into('Roads'));
      report.roads++;
    } catch (e) { report.warnings.push(`road ${r.id}: ${(e as Error).message}`); }
  }

  // scripts listed in the manifest come back as Script objects (attached ones under their object)
  for (const s of (man.scripts ?? []) as { path: string; name: string; kind: string; target: string }[]) {
    const src = files[s.path];
    if (src === undefined) continue;
    const parent = s.target ? ws.getDescendants().find((d) => d.props.MapId === s.target) : undefined;
    const sc = new Instance(s.kind === 'LocalScript' ? 'LocalScript' : s.kind === 'ModuleScript' ? 'ModuleScript' : 'Script');
    sc.props.Name = s.name;
    sc.props.Source = src.replace(/^--[^\n]*\n/, '');
    sc.setParent(parent ?? doc.dm.getService('ServerScriptService')!);
  }

  const cam = ws.findFirstChildOfClass('Camera');
  const h0 = doc.dm.terrainInstance?.terrain?.heightAt(0, 0) ?? 0;
  if (cam) { cam.props.Position = v3(0, h0 + 40, 70); cam.props.Orientation = v3(-30, 0, 0); }
  doc.camera = { position: [0, h0 + 60, 110], yaw: 0, pitch: -0.45 };
  doc.history.clear();
  doc.history.markSaved();
  return { doc, report };
}

const MAP_README = `# worldridr map

A map for the driving simulator [worldridr](https://github.com/flqriann/worldridr), edited with **WorldRidr Studio**.

## What is in here?

The base world is **OpenStreetMap** – the game streams roads, buildings, water and terrain for the whole planet by itself.
This repository only holds what people **add or change** on top of it, so it stays small and merges well:

- \`worldridr-map.json\` – manifest (origin, tile zoom, scripts)
- \`tiles/z15_x_y.json\` – one file per map tile, one object or road per line, sorted by id (small diffs, few merge conflicts)
- \`terrain.json\` – optional custom height model
- \`scripts/\` – Lua scripts (shops, triggers, ...) attached to objects

## Working together

1. Clone this repository in WorldRidr Studio (*File → Map (Git) → Clone…*) or with \`git clone\`.
2. Build, then *Map (Git) → Commit & Push*. Different areas are different tile files, so people rarely collide.
3. Larger changes: use a branch and open a pull request.
4. Start the game with \`WR_MAPDIR=<this folder>\` (or put the folder at \`maps/world\`).

If git reports a conflict inside a tile file, keep both lines (every object/road is one line with a unique id), remove the conflict markers and commit.
`;

export { roadTypeIndex };
