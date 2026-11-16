// "Import a place": geocode -> terrain + buildings + roads + water from real data,
// turned into editable Instances (Terrain, Building, Road) in a new or the current project.

import { Instance } from '../core/Instance';
import { v3, type Vec3 } from '../core/types';
import type { Document } from '../editor/Document';
import { createFromTemplate } from '../project/templates';
import { fetchOsm, interpretBuilding, roadTypeFor, type OsmWay } from './osm';
import { makeGeo, toLocal, type GeoRef } from './geo';
import { fitFootprint, signedArea } from './shapes';
import { importElevation } from './terrainImport';

export interface ImportOptions {
  name: string;
  lat: number;
  lon: number;
  radius: number;
  terrain: boolean;
  buildings: boolean;
  roads: boolean;
  water: boolean;
  areas: boolean;
  /** 'new' creates a project; 'add' puts the data into `target` (same origin as the project). */
  mode: 'new' | 'add';
  target?: Document;
}

export interface ImportStats { buildings: number; roads: number; water: number; areas: number; terrain: boolean }

const MAX_BUILDINGS = 25000;
const r3 = (v: number) => Math.round(v * 1000) / 1000;

function folder(parent: Instance, name: string): Instance {
  let f = parent.children.find((c) => c.className === 'Folder' && c.name === name);
  if (!f) { f = new Instance('Folder'); f.props.Name = name; f.setParent(parent); }
  return f;
}

/** Deterministic pseudo-random in 0..1 from an id. */
const rndOf = (id: number) => { const x = Math.sin(id * 12.9898) * 43758.5453; return x - Math.floor(x); };

function simplify(pts: number[][], tol: number): number[][] {
  if (pts.length < 3) return pts;
  const keep = new Array(pts.length).fill(false);
  keep[0] = keep[pts.length - 1] = true;
  const stack: [number, number][] = [[0, pts.length - 1]];
  while (stack.length) {
    const [a, b] = stack.pop()!;
    let md = 0, mi = -1;
    for (let i = a + 1; i < b; i++) {
      const d = pointSegDist(pts[i], pts[a], pts[b]);
      if (d > md) { md = d; mi = i; }
    }
    if (mi >= 0 && md > tol) { keep[mi] = true; stack.push([a, mi], [mi, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}
function pointSegDist(p: number[], a: number[], b: number[]) {
  const dx = b[0] - a[0], dz = b[1] - a[1];
  const l2 = dx * dx + dz * dz || 1;
  const t = Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2));
  return Math.hypot(p[0] - (a[0] + dx * t), p[1] - (a[1] + dz * t));
}

const AREA_COLORS: Record<string, string> = { forest: '#4d7a44', grass: '#7aa85c', meadow: '#86b062', recreation_ground: '#7aa85c', village_green: '#7aa85c', cemetery: '#8aa07a', park: '#6fa35a', garden: '#74a85e', pitch: '#8fb070' };

export interface GeoContext { geo: GeoRef; baseElevation: number }

/** Reads the project's geo origin from the Workspace, or null when it has none yet. */
export function projectGeo(doc: Document): GeoContext | null {
  const ws = doc.dm.workspace.props;
  if (!ws.PlaceName) return null;
  return { geo: makeGeo(ws.GeoLat as number, ws.GeoLon as number), baseElevation: ws.GeoElevation as number };
}

export async function importPlace(opts: ImportOptions, onStatus: (s: string) => void): Promise<{ doc: Document; stats: ImportStats }> {
  let doc: Document;
  let ctx: GeoContext;
  const stats: ImportStats = { buildings: 0, roads: 0, water: 0, areas: 0, terrain: false };
  if (opts.mode === 'add' && opts.target) {
    doc = opts.target;
    ctx = projectGeo(doc) ?? { geo: makeGeo(opts.lat, opts.lon), baseElevation: 0 };
  } else {
    doc = createFromTemplate('empty');
    doc.name = opts.name;
    ctx = { geo: makeGeo(opts.lat, opts.lon), baseElevation: 0 };
    if (opts.terrain) {
      const el = await importElevation(ctx.geo, opts.radius, onStatus);
      doc.dm.terrainInstance!.terrain = el.terrain;
      ctx.baseElevation = el.baseElevation;
      stats.terrain = true;
    }
    const ws = doc.dm.workspace;
    ws.props.Gravity = 9.81;
    ws.props.GeoLat = opts.lat;
    ws.props.GeoLon = opts.lon;
    ws.props.GeoElevation = ctx.baseElevation;
    ws.props.PlaceName = opts.name;
    ws.props.FallenPartsDestroyHeight = -2000;
    doc.dm.getService('Lighting')!.props.FogEnd = 4000;
    const sp = doc.dm.getService('StarterPlayer')!.props;
    sp.CharacterWalkSpeed = 6; sp.CharacterJumpPower = 5;
  }
  const dm = doc.dm;
  const terrain = dm.terrainInstance?.terrain;
  const groundAt = (x: number, z: number) => (terrain ? terrain.heightAt(x, z) ?? 0 : 0);

  let data = null;
  if (opts.buildings || opts.roads || opts.water || opts.areas) {
    data = await fetchOsm(opts.lat, opts.lon, opts.radius * 1.05, opts, onStatus);
  }
  onStatus('Creating objects…');
  const adding = opts.mode === 'add';
  if (adding) doc.history.begin('Import place');
  try {
  const ws = dm.workspace;
  const [ox, oz] = toLocal(ctx.geo, opts.lat, opts.lon); // center of this import (≠ 0,0 when adding)
  const inRange = (x: number, z: number) => Math.hypot(x - ox, z - oz) <= opts.radius * 1.15;
  const bFolder = () => folder(ws, 'Buildings');
  let bf: Instance | null = null, rf: Instance | null = null, wf: Instance | null = null, af: Instance | null = null;

  for (const way of data?.ways ?? []) {
    const t = way.tags;
    const poly = closedPolygon(way, ctx.geo);
    if (t.building && opts.buildings && poly) {
      if (stats.buildings >= MAX_BUILDINGS) continue;
      if (!inRange(poly[0], poly[1])) continue;
      const area = Math.abs(signedArea(poly));
      if (area < 8) continue;
      const fit = fitFootprint(poly);
      const info = interpretBuilding(t, area, rndOf(way.id));
      const gs = groundStats(poly, groundAt);
      const base = gs.min - 0.4;
      const top = gs.avg + info.height;
      const sizeY = Math.max(2, top - base);
      const b = new Instance('Building');
      b.props.Name = info.name || (t['addr:street'] ? `${t['addr:street']} ${t['addr:housenumber'] ?? ''}`.trim() : 'Building');
      b.props.Footprint = JSON.stringify(fit.norm);
      b.props.Position = v3(r3(fit.cx), r3(base + sizeY / 2), r3(fit.cz));
      b.props.Orientation = v3(0, r3(fit.yaw), 0);
      b.props.Size = v3(r3(fit.sx), r3(sizeY), r3(fit.sz));
      b.props.Color = info.wall;
      b.props.RoofColor = info.roof;
      b.props.RoofShape = info.roofShape;
      b.props.RoofHeight = r3(Math.min(info.roofHeight, sizeY * 0.9));
      b.props.Levels = info.levels;
      b.props.OsmId = String(way.id);
      b.props.Anchored = true;
      bf ??= bFolder();
      b.setParent(bf);
      stats.buildings++;
    } else if (t.natural === 'water' && opts.water && poly) {
      if (!inRange(poly[0], poly[1])) continue;
      const fit = fitFootprint(poly);
      const gs = groundStats(poly, groundAt);
      const w = new Instance('Building');
      w.props.Name = t.name || 'Water';
      w.props.Kind = 'Water';
      w.props.Footprint = JSON.stringify(fit.norm);
      w.props.Size = v3(r3(fit.sx), 0.3, r3(fit.sz));
      w.props.Position = v3(r3(fit.cx), r3(gs.min - 0.1), r3(fit.cz));
      w.props.Orientation = v3(0, r3(fit.yaw), 0);
      w.props.Color = '#3d78a8'; w.props.Material = 'Glass'; w.props.CanCollide = false; w.props.CastShadow = false;
      w.props.OsmId = String(way.id);
      wf ??= folder(ws, 'Water');
      w.setParent(wf);
      stats.water++;
    } else if ((t.landuse || t.leisure) && opts.areas && poly) {
      if (!inRange(poly[0], poly[1])) continue;
      const key = t.landuse ?? t.leisure;
      const gs = groundStats(poly, groundAt);
      if (gs.max - gs.min > 2.5) continue; // slabs do not follow slopes
      const fit = fitFootprint(poly);
      const a = new Instance('Building');
      a.props.Name = t.name || key;
      a.props.Kind = 'Area';
      a.props.Footprint = JSON.stringify(fit.norm);
      a.props.Size = v3(r3(fit.sx), 0.2, r3(fit.sz));
      a.props.Position = v3(r3(fit.cx), r3(gs.max + 0.02), r3(fit.cz));
      a.props.Orientation = v3(0, r3(fit.yaw), 0);
      a.props.Color = AREA_COLORS[key] ?? '#7aa85c'; a.props.CanCollide = false; a.props.CastShadow = false;
      a.props.OsmId = String(way.id);
      af ??= folder(ws, 'Areas');
      a.setParent(af);
      stats.areas++;
    } else if (t.highway && opts.roads) {
      const type = roadTypeFor(t);
      if (!type) continue;
      const created = makeRoads(way, type, ctx.geo, ox, oz, opts.radius * 1.15, groundAt);
      for (const r of created) { rf ??= folder(ws, 'Roads'); r.setParent(rf); stats.roads++; }
    }
  }

  if (opts.mode === 'new') {
    const sp = new Instance('SpawnLocation');
    sp.props.Name = 'SpawnLocation';
    sp.props.Position = v3(0, r3(groundAt(0, 0) + 0.5), 0);
    sp.props.Size = v3(3, 0.4, 3);
    sp.props.Transparency = 0.6;
    sp.setParent(ws);
    const cam = ws.findFirstChildOfClass('Camera');
    if (cam) { cam.props.Position = v3(0, r3(groundAt(0, 0) + 40), 70); cam.props.Orientation = v3(-30, 0, 0); }
    doc.camera = { position: [0, groundAt(0, 0) + 60, 110], yaw: 0, pitch: -0.45 };
  }
  } finally {
    if (adding) doc.history.end();
  }
  if (!adding) { doc.history.clear(); doc.history.markSaved(); }
  return { doc, stats };
}

function closedPolygon(way: OsmWay, geo: GeoRef): number[] | null {
  const g = way.geometry;
  if (g.length < 4) return null;
  const a = g[0], b = g[g.length - 1];
  if (Math.abs(a.lat - b.lat) > 1e-9 || Math.abs(a.lon - b.lon) > 1e-9) return null;
  const out: number[] = [];
  for (let i = 0; i < g.length - 1; i++) { const [x, z] = toLocal(geo, g[i].lat, g[i].lon); out.push(x, z); }
  return out.length >= 6 ? out : null;
}

function groundStats(poly: number[], groundAt: (x: number, z: number) => number) {
  let min = Infinity, max = -Infinity, sum = 0, n = 0;
  let cx = 0, cz = 0;
  for (let i = 0; i < poly.length; i += 2) {
    const h = groundAt(poly[i], poly[i + 1]);
    min = Math.min(min, h); max = Math.max(max, h); sum += h; n++;
    cx += poly[i]; cz += poly[i + 1];
  }
  const hc = groundAt(cx / n, cz / n);
  min = Math.min(min, hc); max = Math.max(max, hc);
  return { min, max, avg: sum / n };
}

function makeRoads(way: OsmWay, type: string, geo: GeoRef, ox: number, oz: number, range: number, groundAt: (x: number, z: number) => number): Instance[] {
  // split at nodes far outside the import range
  const runs: number[][][] = [];
  let cur: number[][] = [];
  for (const n of way.geometry) {
    const [x, z] = toLocal(geo, n.lat, n.lon);
    if (Math.hypot(x - ox, z - oz) > range * 1.5) { if (cur.length > 1) runs.push(cur); cur = []; continue; }
    cur.push([x, z]);
  }
  if (cur.length > 1) runs.push(cur);
  const out: Instance[] = [];
  const bridge = way.tags.bridge && way.tags.bridge !== 'no';
  for (const run of runs) {
    const pts = simplify(run, 0.6);
    if (pts.length < 2) continue;
    // cumulative length for bridge lift
    const cum = [0];
    for (let i = 1; i < pts.length; i++) cum.push(cum[i - 1] + Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
    const total = cum[cum.length - 1] || 1;
    let x0 = Infinity, x1 = -Infinity, z0 = Infinity, z1 = -Infinity;
    for (const p of pts) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
    const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    const flat: number[] = [];
    pts.forEach((p, i) => {
      const t = cum[i] / total;
      const lift = bridge ? Math.sin(Math.PI * t) * Math.min(6, total / 8) : 0;
      flat.push(r3(p[0] - cx), r3(p[1] - cz), r3(lift));
    });
    const r = new Instance('Road');
    r.props.Name = way.tags.name || way.tags.ref || 'Road';
    r.props.StreetName = way.tags.name ?? '';
    r.props.RoadType = type;
    r.props.Points = JSON.stringify(flat);
    r.props.Position = v3(r3(cx), r3(groundAt(cx, cz)), r3(cz));
    r.props.Size = v3(Math.max(2, r3(x1 - x0)), 0.3, Math.max(2, r3(z1 - z0)));
    r.props.OsmId = String(way.id);
    const mx = way.tags.maxspeed ? parseInt(way.tags.maxspeed, 10) : NaN;
    r.props.MapExtra = JSON.stringify(Number.isFinite(mx) ? { maxspeed: mx } : {});
    out.push(r);
  }
  return out;
}

export type { Vec3 };
